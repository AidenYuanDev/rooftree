/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "World.h"
#include "ArenaDatabase.h"
#include "ArenaPackets.h"
#include "ArenaSession.h"
#include "ArenaSocket.h"
#include "Config.h"
#include "KcpProtocol.h"
#include "Log.h"
#include "Player.h"
#include "Random.h"
#include "Transaction.h"
#include "Unit.h"

namespace
{
constexpr float CoinRadius = 10.0f;

enum WorldEvents : uint32
{
    EVENT_GOLD_RUSH_START = 1,
    EVENT_GOLD_RUSH_END   = 2,
};

enum WorldTaskGroups : uint32
{
    GROUP_COINS = 1,
};

// The client hears the combat core's numbers as they are
static_assert(uint8(CombatEventType::Swing) == uint8(Arena::CombatEventType::Swing) && uint8(Arena::CombatEventType::MAX) == uint8(CombatEventType::Swing));
static_assert(uint8(InterruptReason::OutOfRange) == uint8(Arena::InterruptReason::OutOfRange) && uint8(Arena::InterruptReason::MAX) == uint8(InterruptReason::OutOfRange));

Milliseconds GetDuration(Config const& config, std::string_view path, Milliseconds def)
{
    return Milliseconds(config.Get<uint32>(path, uint32(def.count())));
}
}

World* World::instance()
{
    static World instance;
    return &instance;
}

void World::LoadConfigSettings(Config const& config)
{
    WorldSettings settings;
    settings.Width = config.Get<float>("Arena.World.Width", settings.Width);
    settings.Height = config.Get<float>("Arena.World.Height", settings.Height);
    settings.PlayerSpeed = config.Get<float>("Arena.World.PlayerSpeed", settings.PlayerSpeed);
    settings.ViewDistance = config.Get<float>("Arena.World.ViewDistance", settings.ViewDistance);
    settings.MaxVisible = config.Get<uint32>("Arena.World.MaxVisible", settings.MaxVisible);
    settings.MaxCoins = config.Get<uint32>("Arena.Coins.Max", settings.MaxCoins);
    settings.CoinSpawnInterval = GetDuration(config, "Arena.Coins.SpawnInterval", settings.CoinSpawnInterval);
    settings.GoldRushInterval = GetDuration(config, "Arena.GoldRush.Interval", settings.GoldRushInterval);
    settings.GoldRushDuration = GetDuration(config, "Arena.GoldRush.Duration", settings.GoldRushDuration);
    settings.GoldRushCoinValue = config.Get<uint32>("Arena.GoldRush.CoinValue", settings.GoldRushCoinValue);
    settings.SaveInterval = GetDuration(config, "Arena.SaveInterval", settings.SaveInterval);
    settings.IdleTimeout = GetDuration(config, "Arena.IdleTimeout", settings.IdleTimeout);
    settings.MaxChatLength = config.Get<uint32>("Arena.Chat.MaxLength", settings.MaxChatLength);
    settings.ChatInterval = GetDuration(config, "Arena.Chat.Interval", settings.ChatInterval);
    settings.RespawnTime = GetDuration(config, "Arena.Combat.RespawnTime", settings.RespawnTime);
    settings.LingerTime = GetDuration(config, "Arena.LingerTime", settings.LingerTime);
    settings.Motd = config.Get<std::string>("Arena.Motd", settings.Motd);

    // The field can't shrink under the players while they are on it
    if (!_sessions.empty() && (settings.Width != _settings.Width || settings.Height != _settings.Height))
    {
        RT_LOG_WARN("arena.world", "Arena.World.Width/Height can't change while the server runs, keeping {}x{}", _settings.Width, _settings.Height);
        settings.Width = _settings.Width;
        settings.Height = _settings.Height;
    }

    _settings = std::move(settings);
}

void World::SetInitialWorldSettings()
{
    // Settings are read again every time a task repeats, so a reload applies to the next one
    _scheduler.Schedule(_settings.CoinSpawnInterval, GROUP_COINS, [this](TaskContext context)
    {
        SpawnCoin();
        // Three times as many coins during a gold rush
        context.Repeat(_goldRush ? _settings.CoinSpawnInterval / 3 : _settings.CoinSpawnInterval);
    });

    _scheduler.Schedule(_settings.SaveInterval, [this](TaskContext context)
    {
        SavePlayers();
        context.Repeat(_settings.SaveInterval);
    });

    // Ping the connections so an idle server's database connections don't time out
    _scheduler.Schedule(10min, [](TaskContext context)
    {
        ArenaDatabase.KeepAlive();
        context.Repeat();
    });

    _scheduler.Schedule(10s, [this](TaskContext context)
    {
        LogUpdateStats();
        context.Repeat();
    });

    _events.ScheduleEvent(EVENT_GOLD_RUSH_START, _settings.GoldRushInterval);
}

void World::AddSession(std::shared_ptr<ArenaSession> session)
{
    _addSessionQueue.add(std::move(session));
}

void World::Update(uint32 diff)
{
    auto start = std::chrono::steady_clock::now();

    // A quiet point: no session or player is being updated, so the settings change all at once
    if (_reloadRequested.exchange(false))
    {
        std::string error;
        if (sConfigMgr->Reload(error))
        {
            sLog->LoadFromConfig();
            LoadConfigSettings(*sConfigMgr->GetConfig());
            RT_LOG_INFO("arena.world", "Config reloaded");
        }
        else
            RT_LOG_ERROR("arena.world", "Config reload failed, keeping the current one: {}", error);
    }

    AddQueuedSessions();
    UpdateSessions(diff);
    UpdateLingering(diff);
    UpdatePlayers(diff);
    UpdateVisibility();
    UpdateCombat(diff);
    UpdateEvents(diff);
    _scheduler.Update(Milliseconds(diff));
    SendState();

    auto took = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
    ++_updateStats.Updates;
    _updateStats.Total += took;
    _updateStats.Max = std::max(_updateStats.Max, took);
}

void World::LogUpdateStats()
{
    UpdateStats stats = std::exchange(_updateStats, {});
    if (_sessions.empty() || !stats.Updates)
        return;

    RT_LOG_INFO("arena.world", "{} players, {} updates in 10 s: {} us on average, {} us at most, {} combat events",
        _sessions.size(), stats.Updates, stats.Total.count() / stats.Updates, stats.Max.count(), stats.CombatEvents);
    if (stats.States)
        RT_LOG_INFO("arena.world", "States: {} players and {} bytes in one on average, {}% sent reliably (over a datagram)",
            stats.StatePlayers / stats.States, stats.StateBytes / stats.States, stats.StatesReliable * 100 / stats.States);
}

void World::AddQueuedSessions()
{
    std::shared_ptr<ArenaSession> session;
    while (_addSessionQueue.next(session))
    {
        auto [itr, inserted] = _sessions.try_emplace(session->GetAccountId(), session);
        if (inserted)
        {
            // Back while its player still lingers: that one, as it is
            auto lingering = _lingering.find(session->GetAccountId());
            if (lingering != _lingering.end())
            {
                RT_LOG_INFO("arena.world", "{} is back", session->GetName());
                std::unique_ptr<Player> player = std::move(lingering->second.Character);
                _lingering.erase(lingering);
                session->TakeOverPlayer(std::move(player));
            }
            else
                session->LoadPlayer();
            continue;
        }

        // Logged in again while the old connection is still there: the new one takes over the player,
        // with whatever it collected and not saved yet, the old one kicked
        std::shared_ptr<ArenaSession> old = std::exchange(itr->second, session);
        RT_LOG_INFO("arena.world", "{} logged in again, closing the previous connection", session->GetName());
        old->KickPlayer("logged in from another place");
        if (std::unique_ptr<Player> player = old->ReleasePlayer())
            session->TakeOverPlayer(std::move(player));
        else
            session->LoadPlayer();
    }
}

void World::UpdateSessions(uint32 diff)
{
    for (auto itr = _sessions.begin(); itr != _sessions.end();)
    {
        if (!itr->second->Update(diff))
        {
            RemoveSession(*itr->second);
            itr = _sessions.erase(itr);
        }
        else
            ++itr;
    }
}

void World::RemoveSession(ArenaSession& session)
{
    std::unique_ptr<Player> player = session.ReleasePlayer();
    if (!player)
        return;

    // Standing, doing nothing, in the world and the combat map as it was
    RT_LOG_INFO("arena.world", "{} disconnected, lingering", session.GetName());
    player->SetInput(0.0f, 0.0f);
    player->CancelCast();
    player->StopAttack();
    _lingering[session.GetAccountId()] = { std::move(player), _settings.LingerTime };
}

void World::UpdateLingering(uint32 diff)
{
    for (auto itr = _lingering.begin(); itr != _lingering.end();)
    {
        Lingering& lingering = itr->second;
        lingering.Left -= std::min(lingering.Left, Milliseconds(diff));
        if (lingering.Left > 0ms)
        {
            ++itr;
            continue;
        }
        RemovePlayer(*lingering.Character);
        itr = _lingering.erase(itr);
    }
}

void World::RemovePlayer(Player& player)
{
    RT_LOG_INFO("arena.world", "{} left with {} coins", player.GetName(), player.GetScore());
    // A cast at it fails, its auras on others run out
    _combat.RemoveUnit(player);

    ArenaDatabaseTransaction trans = ArenaDatabase.BeginTransaction();
    player.SaveToDB(trans);
    ArenaDatabase.CommitTransaction(trans);

    SendSystemMessage(Rooftree::StringFormat("{} left", player.GetName()));
}

Player* World::FindPlayer(uint32 id) const
{
    if (auto session = _sessions.find(id); session != _sessions.end())
        return session->second->GetPlayer();
    if (auto lingering = _lingering.find(id); lingering != _lingering.end())
        return lingering->second.Character.get();
    return nullptr;
}

void World::UpdatePlayers(uint32 diff)
{
    for (auto& [id, session] : _sessions)
    {
        Player* player = session->GetPlayer();
        if (!player)
            continue;

        player->UpdateMovement(diff, _settings.PlayerSpeed, _settings.Width, _settings.Height);
        if (!player->IsAlive())
            continue;

        std::erase_if(_coins, [player](Coin const& coin)
        {
            float dx = coin.X - player->GetX();
            float dy = coin.Y - player->GetY();
            float reach = Player::Radius + CoinRadius;
            if (dx * dx + dy * dy > reach * reach)
                return false;

            player->AddCoins(coin.Value);
            return true;
        });
    }
}

void World::UpdateVisibility()
{
    std::vector<VisibilityPoint> points;
    points.reserve(_sessions.size() + _lingering.size());
    for (auto& [id, session] : _sessions)
        if (Player* player = session->GetPlayer())
            points.push_back({ player->GetId(), player->GetX(), player->GetY() });
    // Seen where they stand; they see nothing, nobody is there to be sent it
    for (auto& [id, lingering] : _lingering)
        points.push_back({ lingering.Character->GetId(), lingering.Character->GetX(), lingering.Character->GetY() });
    _visibility.Update(points, _settings.ViewDistance, _settings.MaxVisible);
}

void World::UpdateCombat(uint32 diff)
{
    _combat.Update(diff);

    // What happened since the last update, casts started by this update's messages too
    std::vector<CombatEvent> events = _combat.TakeEvents();
    if (events.empty())
        return;
    _updateStats.CombatEvents += events.size();

    // To each client the events whose caster or target its player sees, in order: one packet each
    std::unordered_map<uint32, std::vector<std::size_t>> heard;
    for (std::size_t i = 0; i < events.size(); ++i)
        for (uint32 unit : { events[i].Caster, events[i].Target })
            for (uint32 viewer : _visibility.GetViewers(unit))
            {
                std::vector<std::size_t>& indexes = heard[viewer];
                if (indexes.empty() || indexes.back() != i)
                    indexes.push_back(i);
            }

    for (auto& [viewer, indexes] : heard)
    {
        auto session = _sessions.find(viewer);
        if (session == _sessions.end() || session->second->GetStatus() != STATUS_IN_GAME)
            continue;

        flatbuffers::FlatBufferBuilder builder;
        std::vector<flatbuffers::Offset<Arena::CombatEvent>> log;
        log.reserve(indexes.size());
        for (std::size_t i : indexes)
        {
            CombatEvent const& event = events[i];
            log.push_back(Arena::CreateCombatEvent(builder, Arena::CombatEventType(event.Type), event.Caster, event.Target, event.Spell,
                event.Amount, event.Duration, event.Crit, event.Periodic, Arena::InterruptReason(event.Reason)));
        }
        builder.Finish(Arena::CreateCombatLogDirect(builder, &log));
        session->second->Send({ Arena::Opcode::CombatLog, builder });
    }

    // After the log: who killed whom is told once the death is
    for (CombatEvent const& event : events)
        if (event.Type == CombatEventType::Death)
            OnDeath(event.Target, event.Caster);
}

void World::OnDeath(uint32 victimId, uint32 killerId)
{
    Player* victim = FindPlayer(victimId);
    if (!victim)
        return;

    victim->AddDeath();
    if (Player* killer = FindPlayer(killerId))
    {
        killer->AddKill();
        SendSystemMessage(Rooftree::StringFormat("{} was slain by {}", victim->GetName(), killer->GetName()));
    }
    else
        SendSystemMessage(Rooftree::StringFormat("{} died", victim->GetName()));

    // By account: after a relog it is the new session's, while lingering still the old one
    _scheduler.Schedule(_settings.RespawnTime, [this, victimId](TaskContext)
    {
        Respawn(victimId);
    });
}

void World::Respawn(uint32 accountId)
{
    Player* player = FindPlayer(accountId);
    if (!player || player->IsAlive())
        return;

    player->Revive();
    player->Relocate(frand(Player::Radius, _settings.Width - Player::Radius), frand(Player::Radius, _settings.Height - Player::Radius));
}

void World::UpdateEvents(uint32 diff)
{
    _events.Update(diff);
    while (uint32 eventId = _events.ExecuteEvent())
    {
        switch (eventId)
        {
            case EVENT_GOLD_RUSH_START:
                _goldRush = true;
                SendSystemMessage(Rooftree::StringFormat("Gold rush! New coins are worth {} for {} seconds.",
                    _settings.GoldRushCoinValue, std::chrono::duration_cast<Seconds>(_settings.GoldRushDuration).count()));
                // Spawn the first rush coin now instead of after the normal interval
                _scheduler.RescheduleGroup(GROUP_COINS, 0ms);
                _events.ScheduleEvent(EVENT_GOLD_RUSH_END, _settings.GoldRushDuration);
                break;
            case EVENT_GOLD_RUSH_END:
                _goldRush = false;
                SendSystemMessage("The gold rush is over.");
                _events.ScheduleEvent(EVENT_GOLD_RUSH_START, _settings.GoldRushInterval);
                break;
            default:
                break;
        }
    }
}

void World::SpawnCoin()
{
    if (_coins.size() >= _settings.MaxCoins)
        return;

    _coins.push_back(
    {
        .Id = _nextCoinId++,
        .X = frand(CoinRadius, _settings.Width - CoinRadius),
        .Y = frand(CoinRadius, _settings.Height - CoinRadius),
        .Value = _goldRush ? _settings.GoldRushCoinValue : 1,
    });
}

void World::SavePlayers()
{
    // One transaction for everyone, queued to the async worker: the world thread doesn't wait
    ArenaDatabaseTransaction trans = ArenaDatabase.BeginTransaction();
    for (auto& [id, session] : _sessions)
        if (Player* player = session->GetPlayer(); player && player->IsSaveNeeded())
            player->SaveToDB(trans);
    for (auto& [id, lingering] : _lingering)
        if (lingering.Character->IsSaveNeeded())
            lingering.Character->SaveToDB(trans);

    if (trans->GetSize())
        ArenaDatabase.CommitTransaction(trans);
}

void World::SaveAllPlayersDirect()
{
    ArenaDatabaseTransaction trans = ArenaDatabase.BeginTransaction();
    for (auto& [id, session] : _sessions)
        if (Player* player = session->GetPlayer())
            player->SaveToDB(trans);
    for (auto& [id, lingering] : _lingering)
        lingering.Character->SaveToDB(trans);

    if (trans->GetSize())
        ArenaDatabase.DirectCommitTransaction(trans);
}

void World::KickAll(std::string_view reason)
{
    for (auto& [id, session] : _sessions)
        session->KickPlayer(reason, Rooftree::Net::WebSocket::CloseCode::GoingAway);
}

bool World::AreAllSocketsClosed() const
{
    // Kicked sessions close their socket once the close frame is written
    for (auto& [id, session] : _sessions)
        if (session->IsSocketOpen())
            return false;
    return true;
}

void World::ClearSessions()
{
    _sessions.clear();
    _lingering.clear();

    std::shared_ptr<ArenaSession> session;
    while (_addSessionQueue.next(session))
        ;
}

void World::SendSystemMessage(std::string_view text)
{
    SendGlobalMessage(MakeSystemMessage(text));
}

void World::SendGlobalMessage(ServerPacket const& packet, bool unreliable /*= false*/)
{
    for (auto& [id, session] : _sessions)
        if (session->GetStatus() == STATUS_IN_GAME)
            session->Send(packet, unreliable);
}

void World::SendState()
{
    float const range = _settings.ViewDistance * _settings.ViewDistance;
    std::vector<flatbuffers::Offset<Arena::PlayerState>> players;
    std::vector<flatbuffers::Offset<Arena::CoinState>> coins;

    // Each client its own: what its player sees
    for (auto& [id, session] : _sessions)
    {
        Player* viewer = session->GetPlayer();
        if (!viewer || session->GetStatus() != STATUS_IN_GAME)
            continue;

        flatbuffers::FlatBufferBuilder builder;
        players.clear();
        for (uint32 visibleId : _visibility.GetVisible(viewer->GetId()))
        {
            Player* player = FindPlayer(visibleId);
            if (!player)
                continue;
            players.push_back(Arena::CreatePlayerStateDirect(builder, player->GetId(), player->GetName().c_str(), player->GetX(), player->GetY(),
                player->GetScore(), player->GetTotalCoins(), player->GetHealth(), player->GetMaxHealth(), player->GetMana(), player->GetMaxMana(),
                player->GetTargetId(), player->GetKills(), player->GetDeaths(), player->GetFacing(), player->GetAttackTargetId()));
        }

        coins.clear();
        for (Coin const& coin : _coins)
        {
            float dx = coin.X - viewer->GetX();
            float dy = coin.Y - viewer->GetY();
            if (dx * dx + dy * dy <= range)
                coins.push_back(Arena::CreateCoinState(builder, coin.Id, coin.X, coin.Y, coin.Value));
        }

        builder.Finish(Arena::CreateStateDirect(builder, &players, &coins, _goldRush));
        ServerPacket packet(Arena::Opcode::State, builder);
        ++_updateStats.States;
        _updateStats.StateBytes += packet.GetPacket().size();
        _updateStats.StatePlayers += players.size();
        if (packet.GetPacket().size() > Rooftree::Net::Kcp::MaxBodySize)
            ++_updateStats.StatesReliable;

        // Every tick, the next one replaces it: over KCP it may be lost rather than hold up the ones after it
        session->Send(packet, true);
    }
}
