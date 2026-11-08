/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "World.h"
#include "ArenaDatabase.h"
#include "ArenaPackets.h"
#include "ArenaSession.h"
#include "ArenaSocket.h"
#include "Config.h"
#include "Log.h"
#include "Player.h"
#include "Random.h"
#include "Transaction.h"

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
    settings.MaxCoins = config.Get<uint32>("Arena.Coins.Max", settings.MaxCoins);
    settings.CoinSpawnInterval = GetDuration(config, "Arena.Coins.SpawnInterval", settings.CoinSpawnInterval);
    settings.GoldRushInterval = GetDuration(config, "Arena.GoldRush.Interval", settings.GoldRushInterval);
    settings.GoldRushDuration = GetDuration(config, "Arena.GoldRush.Duration", settings.GoldRushDuration);
    settings.GoldRushCoinValue = config.Get<uint32>("Arena.GoldRush.CoinValue", settings.GoldRushCoinValue);
    settings.SaveInterval = GetDuration(config, "Arena.SaveInterval", settings.SaveInterval);
    settings.IdleTimeout = GetDuration(config, "Arena.IdleTimeout", settings.IdleTimeout);
    settings.MaxChatLength = config.Get<uint32>("Arena.Chat.MaxLength", settings.MaxChatLength);
    settings.ChatInterval = GetDuration(config, "Arena.Chat.Interval", settings.ChatInterval);
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

    _events.ScheduleEvent(EVENT_GOLD_RUSH_START, _settings.GoldRushInterval);
}

void World::AddSession(std::shared_ptr<ArenaSession> session)
{
    _addSessionQueue.add(std::move(session));
}

void World::Update(uint32 diff)
{
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
    UpdatePlayers(diff);
    UpdateEvents(diff);
    _scheduler.Update(Milliseconds(diff));
    SendState();
}

void World::AddQueuedSessions()
{
    std::shared_ptr<ArenaSession> session;
    while (_addSessionQueue.next(session))
    {
        auto [itr, inserted] = _sessions.try_emplace(session->GetAccountId(), session);
        if (inserted)
        {
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
    if (Player* player = session.GetPlayer())
    {
        RT_LOG_INFO("arena.world", "{} left with {} coins", session.GetName(), player->GetScore());

        ArenaDatabaseTransaction trans = ArenaDatabase.BeginTransaction();
        player->SaveToDB(trans);
        ArenaDatabase.CommitTransaction(trans);

        SendSystemMessage(Rooftree::StringFormat("{} left", session.GetName()));
    }
}

void World::UpdatePlayers(uint32 diff)
{
    for (auto& [id, session] : _sessions)
    {
        Player* player = session->GetPlayer();
        if (!player)
            continue;

        player->Update(diff, _settings.PlayerSpeed, _settings.Width, _settings.Height);

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

    if (trans->GetSize())
        ArenaDatabase.CommitTransaction(trans);
}

void World::SaveAllPlayersDirect()
{
    ArenaDatabaseTransaction trans = ArenaDatabase.BeginTransaction();
    for (auto& [id, session] : _sessions)
        if (Player* player = session->GetPlayer())
            player->SaveToDB(trans);

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
    flatbuffers::FlatBufferBuilder builder;

    std::vector<flatbuffers::Offset<Arena::PlayerState>> players;
    for (auto& [id, session] : _sessions)
        if (Player* player = session->GetPlayer())
            players.push_back(Arena::CreatePlayerStateDirect(builder, player->GetId(), player->GetName().c_str(), player->GetX(), player->GetY(),
                player->GetScore(), player->GetTotalCoins()));

    if (players.empty())
        return;

    std::vector<flatbuffers::Offset<Arena::CoinState>> coins;
    coins.reserve(_coins.size());
    for (Coin const& coin : _coins)
        coins.push_back(Arena::CreateCoinState(builder, coin.Id, coin.X, coin.Y, coin.Value));

    builder.Finish(Arena::CreateStateDirect(builder, &players, &coins, _goldRush));

    // Every tick, the next one replaces it: over KCP it may be lost rather than hold up the ones after it
    SendGlobalMessage({ Arena::Opcode::State, builder }, true);
}
