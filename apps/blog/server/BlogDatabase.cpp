/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "BlogDatabase.h"
#include "DatabaseWorkerPool.h"

// Languages are listed "zh" first everywhere: ORDER BY lang DESC, there are only the two
// Columns of a user row, in the order UserRow::FromFields (BlogTypes.cpp) reads them
#define BLOG_USER_COLUMNS "u.id, u.username, u.display_name, u.email, u.email_verified_at IS NOT NULL, u.password_hash, u.role, " \
    "u.locale, u.totp_secret, u.totp_last_step, u.banned_until, u.ban_reason, u.avatar, u.denied, u.deleted_at IS NOT NULL, " \
    "u.granted, u.drive_quota, u.drive_trash_days, u.reply_mails, EXISTS (SELECT 1 FROM post_editor pe WHERE pe.user_id = u.id)"

// Columns of a user in the admin's list, in the order AdminUserRow::FromFields (AdminUsers.cpp) reads them
#define BLOG_ADMIN_USER_COLUMNS "u.id, u.username, u.display_name, u.email, u.role, u.email_verified_at IS NOT NULL, u.banned_until, " \
    "u.ban_reason, u.created_at, (SELECT COUNT(*) FROM comment c WHERE c.user_id = u.id), u.avatar, u.denied, u.deleted_at IS NOT NULL, " \
    "u.last_seen_at, u.online_seconds, u.totp_secret IS NOT NULL, u.granted, u.drive_quota, " \
    "(SELECT CAST(COALESCE(SUM(f.size), 0) AS bigint) FROM drive_file f WHERE f.user_id = u.id) " \
    "+ (SELECT CAST(COALESCE(SUM(pu.size), 0) AS bigint) FROM upload pu WHERE pu.user_id = u.id AND pu.post_id IS NOT NULL), u.drive_trash_days"

#define BLOG_POST_TAGS "(SELECT string_agg(tg.slug, ',' ORDER BY tg.slug) FROM post_tag pt JOIN tag tg ON tg.id = pt.tag_id WHERE pt.post_id = p.id)"
#define BLOG_POST_LANGS "(SELECT string_agg(x.lang, ',' ORDER BY x.lang DESC) FROM post_translation x WHERE x.post_id = p.id)"
#define BLOG_POST_COMMENT_COUNT "(SELECT COUNT(*) FROM comment c WHERE c.target_kind = 'post' AND c.target_id = p.id AND c.status = 'visible')"
#define BLOG_NOTE_LANGS "(SELECT string_agg(x.lang, ',' ORDER BY x.lang DESC) FROM note_translation x WHERE x.note_id = n.id)"
#define BLOG_NOTE_COMMENT_COUNT "(SELECT COUNT(*) FROM comment c WHERE c.target_kind = 'note' AND c.target_id = n.id AND c.status = 'visible')"
#define BLOG_THINKING_LANGS "(SELECT string_agg(x.lang, ',' ORDER BY x.lang DESC) FROM thinking_translation x WHERE x.thinking_id = h.id)"
#define BLOG_THINKING_COMMENT_COUNT "(SELECT COUNT(*) FROM comment c WHERE c.target_kind = 'thinking' AND c.target_id = h.id AND c.status = 'visible')"

// A row's text in the requested language (t, its ? is the requested language); rows without one are
// left out: each language's site shows only what is written in it
#define BLOG_NOTE_TEXT_JOINS "FROM note n JOIN note_translation t ON t.note_id = n.id AND t.lang = ? "
#define BLOG_POST_TEXT_JOINS "FROM post p JOIN post_translation t ON t.post_id = p.id AND t.lang = ? "
#define BLOG_THINKING_TEXT_JOINS "FROM thinking h JOIN thinking_translation t ON t.thinking_id = h.id AND t.lang = ? "

BlogDatabaseConnection::BlogDatabaseConnection(DatabaseConnectionInfo const& info) : DatabaseConnection(info)
{
}

BlogDatabaseConnection::~BlogDatabaseConnection() = default;

void BlogDatabaseConnection::DoPrepareStatements()
{

    // users
    // Deleted accounts (deleted_at) can't log in or be mailed
    PrepareStatement(BLOG_SEL_USER_FOR_LOGIN, "SELECT " BLOG_USER_COLUMNS " FROM users u WHERE (u.username = ? OR u.email = ?) AND u.deleted_at IS NULL LIMIT 1");
    PrepareStatement(BLOG_SEL_USER_BY_ID, "SELECT " BLOG_USER_COLUMNS " FROM users u WHERE u.id = ?");
    PrepareStatement(BLOG_SEL_USER_BY_EMAIL, "SELECT " BLOG_USER_COLUMNS " FROM users u WHERE u.email = ? AND u.deleted_at IS NULL");
    PrepareStatement(BLOG_SEL_USER_CONFLICTS, "SELECT username = ?, email = ? FROM users WHERE username = ? OR email = ?");
    // IGNORE: a concurrent registration of the same name or address loses, the caller checks whose row it is
    PrepareStatement(BLOG_INS_USER, "INSERT INTO users (username, display_name, email, password_hash, role, locale, email_verified_at, totp_secret) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT DO NOTHING");
    PrepareStatement(BLOG_SEL_USER_BY_IDENTITY, "SELECT " BLOG_USER_COLUMNS " FROM user_identity i JOIN users u ON u.id = i.user_id "
        "WHERE i.provider = ? AND i.provider_user_id = ?");
    // IGNORE: the identity is someone's already, or the account has one of the provider's; the caller checks
    PrepareStatement(BLOG_INS_IDENTITY, "INSERT INTO user_identity (user_id, provider, provider_user_id, login, avatar_url) VALUES (?, ?, ?, ?, ?) ON CONFLICT DO NOTHING");
    // At every login and link: the login and picture as the provider has them now
    PrepareStatement(BLOG_UPD_IDENTITY_PROFILE, "UPDATE user_identity SET login = ?, avatar_url = ? WHERE provider = ? AND provider_user_id = ?");
    PrepareStatement(BLOG_SEL_USER_IDENTITIES, "SELECT provider, login, created_at, avatar_url IS NOT NULL FROM user_identity WHERE user_id = ? ORDER BY provider");
    PrepareStatement(BLOG_SEL_IDENTITY_AVATAR_URL, "SELECT avatar_url FROM user_identity WHERE user_id = ? AND provider = ?");
    PrepareStatement(BLOG_SEL_USER_AVATAR, "SELECT avatar FROM users WHERE id = ?");
    PrepareStatement(BLOG_UPD_USER_AVATAR, "UPDATE users SET avatar = ? WHERE id = ?");
    PrepareStatement(BLOG_DEL_IDENTITY, "DELETE FROM user_identity WHERE user_id = ? AND provider = ?");
    PrepareStatement(BLOG_SEL_USER_ID_AND_HASH_BY_NAME, "SELECT id, password_hash FROM users WHERE username = ?");
    PrepareStatement(BLOG_UPD_USER_PASSWORD, "UPDATE users SET password_hash = ? WHERE id = ?");
    PrepareStatement(BLOG_UPD_USER_PROFILE, "UPDATE users SET display_name = ?, locale = ?, reply_mails = ? WHERE id = ?");
    PrepareStatement(BLOG_UPD_USER_VERIFIED, "UPDATE users SET email_verified_at = utc_now() WHERE id = ? AND email_verified_at IS NULL");
    PrepareStatement(BLOG_UPD_USER_TOTP_STEP, "UPDATE users SET totp_last_step = ? WHERE id = ?");
    PrepareStatement(BLOG_UPD_USER_TOTP_PENDING, "UPDATE users SET totp_pending_secret = ? WHERE id = ?");
    PrepareStatement(BLOG_SEL_USER_TOTP_PENDING, "SELECT totp_pending_secret FROM users WHERE id = ?");
    PrepareStatement(BLOG_UPD_USER_TOTP_ENABLE, "UPDATE users SET totp_secret = totp_pending_secret, totp_pending_secret = NULL, totp_last_step = ? "
        "WHERE id = ? AND totp_pending_secret IS NOT NULL");
    PrepareStatement(BLOG_UPD_USER_TOTP_DISABLE, "UPDATE users SET totp_secret = NULL, totp_pending_secret = NULL, totp_last_step = NULL "
        "WHERE id = ?");
    // The console's account set 2fa / delete. Posts and uploads keep their user (no ON DELETE)
    PrepareStatement(BLOG_UPD_USER_TOTP_SET, "UPDATE users SET totp_secret = ?, totp_pending_secret = NULL, totp_last_step = NULL WHERE id = ?");
    PrepareStatement(BLOG_SEL_USER_OWNED_CONTENT, "SELECT (SELECT COUNT(*) FROM post WHERE author_id = ?), (SELECT COUNT(*) FROM upload WHERE user_id = ?)");
    PrepareStatement(BLOG_DEL_USER, "DELETE FROM users WHERE id = ?");
    // The console's account set username / email (the console vouches for the address) / name, banlist account
    PrepareStatement(BLOG_UPD_USER_USERNAME, "UPDATE users SET username = ? WHERE id = ?");
    PrepareStatement(BLOG_UPD_USER_EMAIL, "UPDATE users SET email = ?, email_verified_at = utc_now() WHERE id = ?");
    PrepareStatement(BLOG_UPD_USER_DISPLAY_NAME, "UPDATE users SET display_name = ? WHERE id = ?");
    PrepareStatement(BLOG_SEL_BANNED_USERS, "SELECT id, username, banned_until, ban_reason FROM users WHERE banned_until > utc_now() "
        "ORDER BY banned_until LIMIT 100");
    PrepareStatement(BLOG_SEL_ADMIN_EXISTS, "SELECT COUNT(*) FROM users WHERE role = 'admin'");
    PrepareStatement(BLOG_SEL_FIRST_ADMIN, "SELECT " BLOG_USER_COLUMNS " FROM users u WHERE u.role = 'admin' AND u.deleted_at IS NULL ORDER BY u.id LIMIT 1");

    // sessions and mailed tokens
    PrepareStatement(BLOG_INS_SESSION, "INSERT INTO auth_session (id_hash, user_id, csrf_token, expires_at, ip, user_agent) VALUES (?, ?, ?, ?, ?, ?)");
    PrepareStatement(BLOG_SEL_SESSION, "SELECT s.csrf_token, s.expires_at, s.last_seen_at, " BLOG_USER_COLUMNS " FROM auth_session s "
        "JOIN users u ON u.id = s.user_id WHERE s.id_hash = ? AND s.expires_at > utc_now()");
    PrepareStatement(BLOG_UPD_SESSION_SEEN, "UPDATE auth_session SET last_seen_at = utc_now(), expires_at = ? WHERE id_hash = ?");
    PrepareStatement(BLOG_DEL_SESSION, "DELETE FROM auth_session WHERE id_hash = ?");
    PrepareStatement(BLOG_DEL_USER_SESSIONS, "DELETE FROM auth_session WHERE user_id = ?");
    PrepareStatement(BLOG_DEL_EXPIRED_SESSIONS, "DELETE FROM auth_session WHERE expires_at <= utc_now()");
    PrepareStatement(BLOG_INS_EMAIL_TOKEN, "INSERT INTO email_token (token_hash, user_id, purpose, expires_at) VALUES (?, ?, ?, ?)");
    PrepareStatement(BLOG_SEL_EMAIL_TOKEN, "SELECT user_id FROM email_token WHERE token_hash = ? AND purpose = ? AND used_at IS NULL "
        "AND expires_at > utc_now()");
    PrepareStatement(BLOG_UPD_EMAIL_TOKEN_USED, "UPDATE email_token SET used_at = utc_now() WHERE token_hash = ?");
    // A new link replaces the earlier ones of its kind
    PrepareStatement(BLOG_DEL_USER_EMAIL_TOKENS, "DELETE FROM email_token WHERE user_id = ? AND purpose = ? AND used_at IS NULL");
    PrepareStatement(BLOG_DEL_EXPIRED_EMAIL_TOKENS, "DELETE FROM email_token WHERE expires_at <= utc_now()");

    // posts, public. Lists show the posts in the requested language, only those
    PrepareStatement(BLOG_SEL_POSTS, "SELECT p.id, p.slug, p.cover, p.published_at, p.updated_at, p.original_lang, "
        "t.lang, t.title, t.summary, "
        BLOG_POST_TAGS ", " BLOG_POST_LANGS ", " BLOG_POST_COMMENT_COUNT " "
        BLOG_POST_TEXT_JOINS "WHERE p.status = 'published' "
        "AND (? = '' OR EXISTS (SELECT 1 FROM post_tag pt2 JOIN tag tg2 ON tg2.id = pt2.tag_id WHERE pt2.post_id = p.id AND tg2.slug = ?)) "
        "AND (p.published_at < ? OR (p.published_at = ? AND p.id < ?)) "
        "ORDER BY p.published_at DESC, p.id DESC LIMIT ?");
    PrepareStatement(BLOG_SEL_POST_BY_SLUG, "SELECT p.id, p.slug, p.status, p.original_lang, p.cover, p.comments_enabled, p.published_at, p.updated_at, "
        "u.display_name, " BLOG_POST_TAGS ", " BLOG_POST_LANGS ", " BLOG_POST_COMMENT_COUNT " "
        "FROM post p JOIN users u ON u.id = p.author_id WHERE p.slug = ?");
    PrepareStatement(BLOG_SEL_POST_TRANSLATION, "SELECT lang, title, summary, body_md FROM post_translation WHERE post_id = ? AND lang = ?");
    PrepareStatement(BLOG_SEL_REDIRECT, "SELECT p.slug FROM slug_redirect r JOIN post p ON p.id = r.post_id WHERE r.old_slug = ?");
    PrepareStatement(BLOG_SEL_PUBLISHED_SLUGS, "SELECT p.slug, p.updated_at, " BLOG_POST_LANGS " FROM post p WHERE p.status = 'published' "
        "ORDER BY p.published_at DESC, p.id DESC");
    // The neighbours of a published post (older, newer) for "previous / next"
    PrepareStatement(BLOG_SEL_POST_PREV, "SELECT p.slug, t.title " BLOG_POST_TEXT_JOINS
        "WHERE p.status = 'published' AND (p.published_at < ? OR (p.published_at = ? AND p.id < ?)) ORDER BY p.published_at DESC, p.id DESC LIMIT 1");
    PrepareStatement(BLOG_SEL_POST_NEXT, "SELECT p.slug, t.title " BLOG_POST_TEXT_JOINS
        "WHERE p.status = 'published' AND (p.published_at > ? OR (p.published_at = ? AND p.id > ?)) ORDER BY p.published_at, p.id LIMIT 1");
    PrepareStatement(BLOG_SEL_PUBLISHED_REDIRECTS, "SELECT r.old_slug, p.slug FROM slug_redirect r JOIN post p ON p.id = r.post_id "
        "WHERE p.status = 'published' ORDER BY r.old_slug");

    // posts, admin
    PrepareStatement(BLOG_SEL_ADMIN_POSTS, "SELECT p.id, p.slug, p.status, p.original_lang, p.updated_at, p.published_at, COALESCE(od.title, o.title), " BLOG_POST_LANGS ", p.publish_at, p.author_id, p.review_requested_at IS NOT NULL "
        "FROM post p JOIN post_translation o ON o.post_id = p.id AND o.lang = p.original_lang "
        "LEFT JOIN post_draft od ON od.post_id = p.id AND od.lang = p.original_lang "
        "WHERE (? = '' OR p.status = ? OR (? = 'review' AND p.review_requested_at IS NOT NULL)) ORDER BY p.updated_at DESC, p.id DESC LIMIT ? OFFSET ?");
    PrepareStatement(BLOG_SEL_POST_BY_ID, "SELECT p.id, p.slug, p.status, p.original_lang, p.cover, p.comments_enabled, p.published_at, p.updated_at, "
        "p.created_at, " BLOG_POST_TAGS ", p.publish_at, p.author_id, u.username, u.display_name, u.avatar, p.review_requested_at "
        "FROM post p JOIN users u ON u.id = p.author_id WHERE p.id = ?");
    PrepareStatement(BLOG_SEL_POST_TRANSLATIONS, "SELECT lang, title, summary, body_md FROM post_translation WHERE post_id = ? ORDER BY lang DESC");
    PrepareStatement(BLOG_SEL_POST_ID_BY_SLUG, "SELECT id FROM post WHERE slug = ?");
    PrepareStatement(BLOG_INS_POST, "INSERT INTO post (slug, status, original_lang, cover, author_id, comments_enabled, published_at, publish_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    // In the transaction that inserted the post: no other sequence used in between, so lastval() is
    // still the post's id
    PrepareStatement(BLOG_INS_NEW_POST_TRANSLATION, "INSERT INTO post_translation (post_id, lang, title, summary, body_md) VALUES (lastval(), ?, ?, ?, ?)");
    PrepareStatement(BLOG_INS_NEW_POST_TAG, "INSERT INTO post_tag (post_id, tag_id) SELECT lastval(), id FROM tag WHERE slug = ?");
    PrepareStatement(BLOG_UPD_POST, "UPDATE post SET slug = ?, status = ?, original_lang = ?, cover = ?, comments_enabled = ?, "
        "published_at = COALESCE(published_at, ?), publish_at = ?, updated_at = utc_now() WHERE id = ?");
    // Drafts whose time came: published by the server, as by the admin
    PrepareStatement(BLOG_SEL_DUE_POSTS, "SELECT p.id, p.slug, o.title FROM post p JOIN post_translation o ON o.post_id = p.id AND o.lang = p.original_lang "
        "WHERE p.status = 'draft' AND p.publish_at <= utc_now() ORDER BY p.publish_at, p.id");
    PrepareStatement(BLOG_UPD_PUBLISH_DUE_POST, "UPDATE post SET status = 'published', published_at = COALESCE(published_at, utc_now()), "
        "publish_at = NULL, updated_at = utc_now() WHERE id = ? AND status = 'draft' AND publish_at <= utc_now()");
    PrepareStatement(BLOG_REP_POST_TRANSLATION, "INSERT INTO post_translation (post_id, lang, title, summary, body_md) VALUES (?, ?, ?, ?, ?) ON CONFLICT (post_id, lang) DO UPDATE SET title = EXCLUDED.title, summary = EXCLUDED.summary, body_md = EXCLUDED.body_md");
    PrepareStatement(BLOG_DEL_POST_TRANSLATION, "DELETE FROM post_translation WHERE post_id = ? AND lang = ?");
    PrepareStatement(BLOG_DEL_POST_TAGS, "DELETE FROM post_tag WHERE post_id = ?");
    PrepareStatement(BLOG_INS_POST_TAG, "INSERT INTO post_tag (post_id, tag_id) SELECT ?::bigint, id FROM tag WHERE slug = ?");
    PrepareStatement(BLOG_REP_REDIRECT, "INSERT INTO slug_redirect (old_slug, post_id) VALUES (?, ?) ON CONFLICT (old_slug) DO UPDATE SET post_id = EXCLUDED.post_id");
    PrepareStatement(BLOG_DEL_REDIRECT, "DELETE FROM slug_redirect WHERE old_slug = ?");
    PrepareStatement(BLOG_DEL_POST, "DELETE FROM post WHERE id = ?");
    PrepareStatement(BLOG_UPD_POST_WORDS, "UPDATE post SET word_count = ? WHERE id = ?");
    // Posts from before word_count, counted at startup
    PrepareStatement(BLOG_SEL_POSTS_WITHOUT_WORDS, "SELECT p.id, o.title, o.body_md FROM post p "
        "JOIN post_translation o ON o.post_id = p.id AND o.lang = p.original_lang WHERE p.word_count = 0");

    // notes: like posts, with numbers instead of slugs. Lists carry the start of the text
    PrepareStatement(BLOG_SEL_NOTES, "SELECT n.id, n.mood, n.weather, n.cover, n.published_at, n.updated_at, n.original_lang, "
        "t.lang, t.title, LEFT(t.body_md, 300), " BLOG_NOTE_LANGS ", " BLOG_NOTE_COMMENT_COUNT " "
        BLOG_NOTE_TEXT_JOINS "WHERE n.status = 'published' AND (n.published_at < ? OR (n.published_at = ? AND n.id < ?)) "
        "ORDER BY n.published_at DESC, n.id DESC LIMIT ?");
    PrepareStatement(BLOG_SEL_NOTE, "SELECT n.id, n.status, n.mood, n.weather, n.cover, n.comments_enabled, n.published_at, n.updated_at, "
        "n.original_lang, n.word_count, " BLOG_NOTE_LANGS ", " BLOG_NOTE_COMMENT_COUNT " FROM note n WHERE n.id = ?");
    PrepareStatement(BLOG_SEL_NOTE_TRANSLATION, "SELECT lang, title, body_md FROM note_translation WHERE note_id = ? AND lang = ?");
    PrepareStatement(BLOG_SEL_NOTE_PREV, "SELECT n.id, t.title " BLOG_NOTE_TEXT_JOINS
        "WHERE n.status = 'published' AND (n.published_at < ? OR (n.published_at = ? AND n.id < ?)) ORDER BY n.published_at DESC, n.id DESC LIMIT 1");
    PrepareStatement(BLOG_SEL_NOTE_NEXT, "SELECT n.id, t.title " BLOG_NOTE_TEXT_JOINS
        "WHERE n.status = 'published' AND (n.published_at > ? OR (n.published_at = ? AND n.id > ?)) ORDER BY n.published_at, n.id LIMIT 1");
    PrepareStatement(BLOG_SEL_PUBLISHED_NOTES, "SELECT n.id, n.updated_at, " BLOG_NOTE_LANGS " FROM note n WHERE n.status = 'published' "
        "ORDER BY n.published_at DESC, n.id DESC");
    PrepareStatement(BLOG_SEL_ADMIN_NOTES, "SELECT n.id, n.status, n.original_lang, n.updated_at, n.published_at, o.title, " BLOG_NOTE_LANGS " "
        "FROM note n JOIN note_translation o ON o.note_id = n.id AND o.lang = n.original_lang "
        "WHERE (? = '' OR n.status = ?) ORDER BY n.updated_at DESC, n.id DESC LIMIT ? OFFSET ?");
    PrepareStatement(BLOG_SEL_NOTE_BY_ID, "SELECT id, status, original_lang, mood, weather, cover, comments_enabled, published_at, updated_at, created_at "
        "FROM note WHERE id = ?");
    PrepareStatement(BLOG_SEL_NOTE_TRANSLATIONS, "SELECT lang, title, body_md FROM note_translation WHERE note_id = ? ORDER BY lang DESC");
    PrepareStatement(BLOG_INS_NOTE, "INSERT INTO note (status, original_lang, mood, weather, cover, comments_enabled, word_count, published_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    PrepareStatement(BLOG_INS_NEW_NOTE_TRANSLATION, "INSERT INTO note_translation (note_id, lang, title, body_md) VALUES (lastval(), ?, ?, ?)");
    // Only the admin creates notes, thinking and projects, one at a time: the newest is the one just made
    PrepareStatement(BLOG_SEL_NEWEST_NOTE_ID, "SELECT MAX(id) FROM note");
    PrepareStatement(BLOG_UPD_NOTE, "UPDATE note SET status = ?, original_lang = ?, mood = ?, weather = ?, cover = ?, comments_enabled = ?, "
        "published_at = COALESCE(published_at, ?), updated_at = utc_now() WHERE id = ?");
    PrepareStatement(BLOG_REP_NOTE_TRANSLATION, "INSERT INTO note_translation (note_id, lang, title, body_md) VALUES (?, ?, ?, ?) ON CONFLICT (note_id, lang) DO UPDATE SET title = EXCLUDED.title, body_md = EXCLUDED.body_md");
    PrepareStatement(BLOG_DEL_NOTE_TRANSLATION, "DELETE FROM note_translation WHERE note_id = ? AND lang = ?");
    PrepareStatement(BLOG_DEL_NOTE, "DELETE FROM note WHERE id = ?");
    PrepareStatement(BLOG_UPD_NOTE_WORDS, "UPDATE note SET word_count = ? WHERE id = ?");
    PrepareStatement(BLOG_SEL_NOTES_WITHOUT_WORDS, "SELECT n.id, o.title, o.body_md FROM note n "
        "JOIN note_translation o ON o.note_id = n.id AND o.lang = n.original_lang WHERE n.word_count = 0");

    // thinking: one-liners, the whole text in lists
    PrepareStatement(BLOG_SEL_THINKING_LIST, "SELECT h.id, h.published_at, h.original_lang, t.lang, t.body_md, "
        BLOG_THINKING_LANGS ", " BLOG_THINKING_COMMENT_COUNT ", h.comments_enabled " BLOG_THINKING_TEXT_JOINS
        "WHERE h.status = 'published' AND (h.published_at < ? OR (h.published_at = ? AND h.id < ?)) "
        "ORDER BY h.published_at DESC, h.id DESC LIMIT ?");
    PrepareStatement(BLOG_SEL_ADMIN_THINKING, "SELECT h.id, h.status, h.original_lang, h.updated_at, h.published_at, o.body_md, " BLOG_THINKING_LANGS " "
        "FROM thinking h JOIN thinking_translation o ON o.thinking_id = h.id AND o.lang = h.original_lang "
        "ORDER BY h.created_at DESC, h.id DESC LIMIT ? OFFSET ?");
    PrepareStatement(BLOG_SEL_THINKING_BY_ID, "SELECT id, status, original_lang, comments_enabled, published_at, updated_at, created_at "
        "FROM thinking WHERE id = ?");
    PrepareStatement(BLOG_SEL_THINKING_TRANSLATIONS, "SELECT lang, body_md FROM thinking_translation WHERE thinking_id = ? ORDER BY lang DESC");
    PrepareStatement(BLOG_INS_THINKING, "INSERT INTO thinking (status, original_lang, comments_enabled, published_at) VALUES (?, ?, ?, ?)");
    PrepareStatement(BLOG_INS_NEW_THINKING_TRANSLATION, "INSERT INTO thinking_translation (thinking_id, lang, body_md) VALUES (lastval(), ?, ?)");
    PrepareStatement(BLOG_SEL_NEWEST_THINKING_ID, "SELECT MAX(id) FROM thinking");
    PrepareStatement(BLOG_UPD_THINKING, "UPDATE thinking SET status = ?, original_lang = ?, comments_enabled = ?, published_at = COALESCE(published_at, ?), "
        "updated_at = utc_now() WHERE id = ?");
    PrepareStatement(BLOG_REP_THINKING_TRANSLATION, "INSERT INTO thinking_translation (thinking_id, lang, body_md) VALUES (?, ?, ?) ON CONFLICT (thinking_id, lang) DO UPDATE SET body_md = EXCLUDED.body_md");
    PrepareStatement(BLOG_DEL_THINKING_TRANSLATION, "DELETE FROM thinking_translation WHERE thinking_id = ? AND lang = ?");
    PrepareStatement(BLOG_DEL_THINKING, "DELETE FROM thinking WHERE id = ?");

    // site. JSON as text: the documents are parsed by Boost.JSON
    PrepareStatement(BLOG_SEL_SITE_SETTINGS, "SELECT name, value::text FROM site_setting");
    PrepareStatement(BLOG_REP_SITE_SETTING, "INSERT INTO site_setting (name, value) VALUES (?, ?) ON CONFLICT (name) DO UPDATE SET value = EXCLUDED.value, updated_at = utc_now()");
    PrepareStatement(BLOG_SEL_SITE_STATS, "SELECT (SELECT COUNT(*) FROM post WHERE status = 'published'), (SELECT COUNT(*) FROM note WHERE status = 'published'), "
        "(SELECT COUNT(*) FROM thinking WHERE status = 'published'), "
        "CAST((SELECT COALESCE(SUM(word_count), 0) FROM post WHERE status = 'published') + "
        "(SELECT COALESCE(SUM(word_count), 0) FROM note WHERE status = 'published') AS bigint)");
    // Everything published, newest first: what, which, its title (thinking: its start) in the language
    PrepareStatement(BLOG_SEL_TIMELINE, "(SELECT 'post' AS kind, p.id, p.slug, t.title AS title, p.published_at "
        BLOG_POST_TEXT_JOINS "WHERE p.status = 'published') "
        "UNION ALL (SELECT 'note', n.id, '', t.title, n.published_at " BLOG_NOTE_TEXT_JOINS "WHERE n.status = 'published') "
        "UNION ALL (SELECT 'thinking', h.id, '', LEFT(t.body_md, 80), h.published_at " BLOG_THINKING_TEXT_JOINS
        "WHERE h.status = 'published') ORDER BY published_at DESC, id DESC LIMIT ?");
    PrepareStatement(BLOG_SEL_FRIENDS, "SELECT id, name, url, avatar, description FROM friend_link WHERE status = 'approved' ORDER BY sort, id");
#define BLOG_FRIEND_COLUMNS "f.id, f.name, f.url, f.avatar, f.description, f.status, f.message, f.reject_reason, f.sort, f.created_at, f.reviewed_at, " \
    "u.id AS applicant_id, u.username, u.display_name, u.email, u.locale"
    PrepareStatement(BLOG_SEL_ADMIN_FRIENDS, "SELECT " BLOG_FRIEND_COLUMNS " FROM friend_link f LEFT JOIN users u ON u.id = f.user_id "
        "WHERE (? = '' OR f.status = ?) ORDER BY f.status = 'pending' DESC, f.sort, f.id DESC");
    PrepareStatement(BLOG_SEL_FRIEND, "SELECT " BLOG_FRIEND_COLUMNS " FROM friend_link f LEFT JOIN users u ON u.id = f.user_id WHERE f.id = ?");
    // A site is listed (or waiting) once; the newest row of an address is also how a new one is found
    PrepareStatement(BLOG_SEL_FRIEND_BY_URL, "SELECT id FROM friend_link WHERE url = ? AND status <> 'rejected' ORDER BY id DESC LIMIT 1");
    PrepareStatement(BLOG_SEL_USER_PENDING_FRIENDS, "SELECT COUNT(*) FROM friend_link WHERE user_id = ? AND status = 'pending'");
    PrepareStatement(BLOG_INS_FRIEND, "INSERT INTO friend_link (name, url, avatar, description, status, user_id, message, sort, reviewed_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    PrepareStatement(BLOG_UPD_FRIEND, "UPDATE friend_link SET name = ?, url = ?, avatar = ?, description = ?, sort = ? WHERE id = ?");
    PrepareStatement(BLOG_UPD_FRIEND_REVIEW, "UPDATE friend_link SET status = ?, reject_reason = ?, reviewed_at = utc_now() WHERE id = ?");
    PrepareStatement(BLOG_DEL_FRIEND, "DELETE FROM friend_link WHERE id = ?");
    PrepareStatement(BLOG_SEL_PROJECTS, "SELECT id, name, url, repo_url, icon, description_zh, description_en, sort FROM project ORDER BY sort, id");
    PrepareStatement(BLOG_SEL_PROJECT, "SELECT id, name, url, repo_url, icon, description_zh, description_en, sort FROM project WHERE id = ?");
    PrepareStatement(BLOG_INS_PROJECT, "INSERT INTO project (name, url, repo_url, icon, description_zh, description_en, sort) VALUES (?, ?, ?, ?, ?, ?, ?)");
    PrepareStatement(BLOG_SEL_NEWEST_PROJECT_ID, "SELECT MAX(id) FROM project");
    PrepareStatement(BLOG_UPD_PROJECT, "UPDATE project SET name = ?, url = ?, repo_url = ?, icon = ?, description_zh = ?, description_en = ?, sort = ? WHERE id = ?");
    PrepareStatement(BLOG_DEL_PROJECT, "DELETE FROM project WHERE id = ?");

    // tags
    // With their counts of published posts: all, in Chinese, in English
#define BLOG_TAG_POST_COUNT(lang) "(SELECT COUNT(*) FROM post_tag pt JOIN post p ON p.id = pt.post_id WHERE pt.tag_id = t.id AND p.status = 'published'" \
    lang ")"
#define BLOG_IN_LANG(lang) " AND EXISTS (SELECT 1 FROM post_translation x WHERE x.post_id = p.id AND x.lang = '" lang "')"
    PrepareStatement(BLOG_SEL_TAGS, "SELECT t.id, t.slug, t.name_zh, t.name_en, " BLOG_TAG_POST_COUNT("") ", "
        BLOG_TAG_POST_COUNT(BLOG_IN_LANG("zh")) ", " BLOG_TAG_POST_COUNT(BLOG_IN_LANG("en")) " FROM tag t ORDER BY t.slug");
    PrepareStatement(BLOG_INS_TAG, "INSERT INTO tag (slug, name_zh, name_en) VALUES (?, ?, ?) ON CONFLICT DO NOTHING");
    PrepareStatement(BLOG_UPD_TAG, "UPDATE tag SET name_zh = ?, name_en = ? WHERE slug = ?");
    PrepareStatement(BLOG_DEL_TAG, "DELETE FROM tag WHERE slug = ?");

    // comments
    PrepareStatement(BLOG_SEL_POST_FOR_COMMENT, "SELECT status, comments_enabled FROM post WHERE id = ?");
    PrepareStatement(BLOG_SEL_NOTE_FOR_COMMENT, "SELECT status, comments_enabled FROM note WHERE id = ?");
    PrepareStatement(BLOG_SEL_THINKING_FOR_COMMENT, "SELECT status, comments_enabled FROM thinking WHERE id = ?");
    PrepareStatement(BLOG_SEL_COMMENT, "SELECT id, target_kind, target_id, user_id, parent_id, status, created_at FROM comment WHERE id = ?");
    PrepareStatement(BLOG_SEL_USER_TARGET_COMMENT_COUNT, "SELECT COUNT(*) FROM comment WHERE user_id = ? AND target_kind = ? AND target_id = ?");
    // The target twice: its kind's foreign key (for ON DELETE CASCADE) and target_kind, target_id
    PrepareStatement(BLOG_INS_COMMENT, "INSERT INTO comment (post_id, note_id, thinking_id, target_kind, target_id, user_id, parent_id, reply_to_user_id, body, ip, "
        "reply_to_id, anchor_lang, anchor_block, anchor_quote, anchor_prefix, anchor_suffix, anchor_text, anchor_revision) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    // A passage chosen again: found in the post as it is
    PrepareStatement(BLOG_UPD_COMMENT_ANCHOR, "UPDATE comment SET anchor_lang = ?, anchor_block = ?, anchor_quote = ?, anchor_prefix = ?, anchor_suffix = ?, "
        "anchor_text = ?, anchor_revision = ?, anchor_status = 'ok' WHERE id = ?");
    // The threads on passages of a post in a language, oldest first, as BLOG_SEL_COMMENT_THREADS keeps them
    PrepareStatement(BLOG_SEL_ANCHORED_THREADS, "SELECT c.id FROM comment c WHERE c.target_kind = 'post' AND c.target_id = ? AND c.parent_id IS NULL "
        "AND c.anchor_lang = ? AND (c.status = 'visible' OR (? = 1 AND c.status = 'hidden') "
        "OR (c.status = 'deleted' AND EXISTS (SELECT 1 FROM comment r WHERE r.parent_id = c.id AND r.status = 'visible'))) "
        "ORDER BY c.id LIMIT 1000");
    PrepareStatement(BLOG_SEL_POST_HAS_LANG, "SELECT 1 FROM post_translation WHERE post_id = ? AND lang = ?");
    PrepareStatement(BLOG_SEL_POST_LATEST_REVISION, "SELECT id FROM post_revision WHERE post_id = ? AND lang = ? AND kind = 'published' "
        "ORDER BY id DESC LIMIT 1");
    // A post's versions and the passages on it (Posts.h WorkerSyncPostRevisions)
    PrepareStatement(BLOG_SEL_POST_TEXTS, "SELECT p.status, t.lang, t.title, t.body_md, t.summary FROM post p JOIN post_translation t ON t.post_id = p.id "
        "WHERE p.id = ?");
    // The newest version of a kind ('' either)
    PrepareStatement(BLOG_SEL_POST_LATEST_REVISION_TEXT, "SELECT title, body_md, summary FROM post_revision WHERE post_id = ? AND lang = ? "
        "AND (? = '' OR kind = ?) ORDER BY id DESC LIMIT 1");
    PrepareStatement(BLOG_INS_POST_REVISION, "INSERT INTO post_revision (post_id, lang, kind, title, summary, body_md, author_id, note) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    PrepareStatement(BLOG_SEL_POST_PASSAGES, "SELECT id, anchor_lang, anchor_quote, anchor_status FROM comment "
        "WHERE target_kind = 'post' AND target_id = ? AND anchor_quote IS NOT NULL");
    PrepareStatement(BLOG_UPD_COMMENT_ANCHOR_STATUS, "UPDATE comment SET anchor_status = ? WHERE id = ?");
    // Every version of a post, newest first, a page of them before an id (0: from the newest)
    PrepareStatement(BLOG_SEL_POST_REVISIONS, "SELECT r.id, r.lang, r.kind, r.created_at, r.title, r.note, CHAR_LENGTH(r.body_md), "
        "u.id AS author_id, u.username, u.display_name, u.avatar FROM post_revision r LEFT JOIN users u ON u.id = r.author_id "
        "WHERE r.post_id = ? AND (? = 0 OR r.id < ?) ORDER BY r.id DESC LIMIT ?");
    // One in full, and the one before it in its language, what it changed
    PrepareStatement(BLOG_SEL_POST_REVISION, "SELECT r.id, r.lang, r.kind, r.created_at, r.title, r.note, CHAR_LENGTH(r.body_md), "
        "u.id AS author_id, u.username, u.display_name, u.avatar, r.summary, r.body_md FROM post_revision r LEFT JOIN users u ON u.id = r.author_id "
        "WHERE r.post_id = ? AND r.id = ?");
    PrepareStatement(BLOG_SEL_POST_REVISION_BEFORE, "SELECT title, summary, body_md FROM post_revision WHERE post_id = ? AND lang = ? AND id < ? "
        "ORDER BY id DESC LIMIT 1");
    // A version back in the working copy: newer than any of its documents (written first), so their
    // pages open it again
    PrepareStatement(BLOG_UPS_POST_DRAFT_RESTORE, "INSERT INTO post_draft (post_id, lang, title, title_v, summary, summary_v, body_md, body_v) "
        "VALUES (?, ?, ?, 1, ?, 1, ?, 1) ON CONFLICT (post_id, lang) DO UPDATE SET title = EXCLUDED.title, title_v = post_draft.title_v + 1, summary = EXCLUDED.summary, "
        "summary_v = post_draft.summary_v + 1, body_md = EXCLUDED.body_md, body_v = post_draft.body_v + 1, updated_at = utc_now()");
    // One user's comments are rate limited and handled one at a time, the newest is the one just inserted
    PrepareStatement(BLOG_SEL_LAST_USER_COMMENT, "SELECT id FROM comment WHERE user_id = ? AND target_kind = ? AND target_id = ? ORDER BY id DESC LIMIT 1");
    // A page of threads, newest first. A deleted top-level comment stays while it has visible replies;
    // the admin (? = 1) sees hidden ones too
    PrepareStatement(BLOG_SEL_COMMENT_THREADS, "SELECT c.id FROM comment c WHERE c.target_kind = ? AND c.target_id = ? AND c.parent_id IS NULL "
        "AND (c.status = 'visible' OR (? = 1 AND c.status = 'hidden') "
        "OR (c.status = 'deleted' AND EXISTS (SELECT 1 FROM comment r WHERE r.parent_id = c.id AND r.status = 'visible'))) "
        "AND c.id < ? ORDER BY c.id DESC LIMIT ?");
    PrepareStatement(BLOG_UPD_COMMENT_BODY, "UPDATE comment SET body = ?, edited_at = utc_now() WHERE id = ?");
    PrepareStatement(BLOG_UPD_COMMENT_STATUS, "UPDATE comment SET status = ? WHERE id = ? AND status <> 'deleted'");
    // The text goes, the row stays: replies keep their thread
    PrepareStatement(BLOG_UPD_COMMENT_DELETED, "UPDATE comment SET status = 'deleted', body = '' WHERE id = ?");
    PrepareStatement(BLOG_SEL_ADMIN_COMMENTS, "SELECT c.id, c.target_kind, c.target_id, p.slug, c.user_id, u.username, u.display_name, c.body, c.status, "
        "c.created_at, c.ip, c.parent_id, u.banned_until, c.anchor_lang, c.anchor_quote, c.anchor_text, c.anchor_status "
        "FROM comment c LEFT JOIN post p ON p.id = c.post_id JOIN users u ON u.id = c.user_id "
        "WHERE (? = '' OR c.status = ?) AND (? = '' OR (? = 'any' AND c.anchor_quote IS NOT NULL) OR (? = 'orphaned' AND c.anchor_status = 'orphaned')) "
        "AND c.id < ? ORDER BY c.id DESC LIMIT ?");
    // Left where its post lost it: still shown under "the text changed", no longer the admin's to decide
    PrepareStatement(BLOG_UPD_COMMENT_PASSAGE_KEPT, "UPDATE comment SET anchor_status = 'kept' WHERE id = ? AND anchor_status = 'orphaned'");

    // A reply's mail: the page of the comment, its title in each language
    PrepareStatement(BLOG_SEL_POST_TITLES, "SELECT x.lang, x.title, p.slug FROM post p JOIN post_translation x ON x.post_id = p.id WHERE p.id = ? ORDER BY x.lang DESC");
    PrepareStatement(BLOG_SEL_NOTE_TITLES, "SELECT lang, title, '' FROM note_translation WHERE note_id = ? ORDER BY lang DESC");

    // reactions. IGNORE: the same reaction twice is still one
    PrepareStatement(BLOG_SEL_COMMENT_FOR_REACTION, "SELECT status FROM comment WHERE id = ?");
    PrepareStatement(BLOG_INS_REACTION, "INSERT INTO reaction (user_id, target_kind, target_id, kind, post_id, note_id, thinking_id, comment_id) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT DO NOTHING");
    PrepareStatement(BLOG_DEL_REACTION, "DELETE FROM reaction WHERE user_id = ? AND target_kind = ? AND target_id = ? AND kind = ?");

    // subscriptions
    PrepareStatement(BLOG_SEL_SUBSCRIBER_BY_EMAIL, "SELECT id, status, unsubscribe_token FROM subscriber WHERE email = ?");
    PrepareStatement(BLOG_SEL_SUBSCRIBER_BY_CONFIRM, "SELECT id FROM subscriber WHERE confirm_hash = ? AND status = 'pending' "
        "AND confirm_expires_at > utc_now()");
    PrepareStatement(BLOG_SEL_SUBSCRIBER_BY_TOKEN, "SELECT id, email FROM subscriber WHERE unsubscribe_token = ?");
    PrepareStatement(BLOG_INS_SUBSCRIBER, "INSERT INTO subscriber (email, lang, status, confirm_hash, confirm_expires_at, unsubscribe_token, confirmed_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?) ON CONFLICT DO NOTHING");
    PrepareStatement(BLOG_UPD_SUBSCRIBER_PENDING, "UPDATE subscriber SET status = 'pending', lang = ?, confirm_hash = ?, confirm_expires_at = ? WHERE id = ?");
    PrepareStatement(BLOG_UPD_SUBSCRIBER_CONFIRMED, "UPDATE subscriber SET status = 'active', confirm_hash = NULL, confirmed_at = utc_now() WHERE id = ?");
    PrepareStatement(BLOG_UPD_SUBSCRIBER_STATUS, "UPDATE subscriber SET status = ?, lang = ?, confirm_hash = NULL WHERE id = ?");
    PrepareStatement(BLOG_UPD_SUBSCRIBER_UNSUBSCRIBED, "UPDATE subscriber SET status = 'unsubscribed', confirm_hash = NULL WHERE id = ?");
    PrepareStatement(BLOG_SEL_ACTIVE_SUBSCRIBERS, "SELECT email, lang, unsubscribe_token FROM subscriber WHERE status = 'active' ORDER BY id");
    // A post or note to mail: published, never mailed. Each translation as "lang", title, summary
    PrepareStatement(BLOG_SEL_POST_TO_MAIL, "SELECT p.slug, t.lang, t.title, t.summary FROM post p JOIN post_translation t ON t.post_id = p.id "
        "WHERE p.id = ? AND p.status = 'published' AND p.mailed_at IS NULL");
    PrepareStatement(BLOG_UPD_POST_MAILED, "UPDATE post SET mailed_at = utc_now() WHERE id = ?");
    PrepareStatement(BLOG_SEL_NOTE_TO_MAIL, "SELECT '', t.lang, t.title, LEFT(t.body_md, 200) FROM note n JOIN note_translation t ON t.note_id = n.id "
        "WHERE n.id = ? AND n.status = 'published' AND n.mailed_at IS NULL");
    PrepareStatement(BLOG_UPD_NOTE_MAILED, "UPDATE note SET mailed_at = utc_now() WHERE id = ?");

    // users and bans, admin
    // Filters, each off with '' (or 0): the search (by address only when the 1st ? is 1, the admin), the
    // role, ids (FIND_IN_SET, the users online), banned; deleted accounts or the others. Newest first, or
    // last seen first when the sort ? is 1
    PrepareStatement(BLOG_SEL_ADMIN_USERS, "SELECT " BLOG_ADMIN_USER_COLUMNS " FROM users u "
        "WHERE (? = '' OR u.username COLLATE \"default\" ILIKE ? OR u.display_name ILIKE ? OR (? = 1 AND u.email COLLATE \"default\" ILIKE ?)) AND (? = '' OR u.role = ?) "
        "AND (? = '' OR u.id::text = ANY(string_to_array(?, ','))) AND (? = 0 OR u.banned_until > utc_now()) AND (u.deleted_at IS NOT NULL) = ? "
        "ORDER BY CASE WHEN ? = 1 THEN u.last_seen_at END DESC NULLS LAST, u.id DESC LIMIT ? OFFSET ?");
    // One of them, with more
    PrepareStatement(BLOG_SEL_ADMIN_USER, "SELECT " BLOG_ADMIN_USER_COLUMNS ", (SELECT COUNT(*) FROM reaction r WHERE r.user_id = u.id), u.locale, "
        "u.password_hash IS NOT NULL FROM users u WHERE u.id = ?");
    PrepareStatement(BLOG_SEL_USER_CONFLICTS_EXCEPT, "SELECT username = ?, email = ? FROM users WHERE (username = ? OR email = ?) AND id <> ?");
    // Everything the admin edits at once. Never another admin, never a deleted account. The address
    // keeps its verification date while it stays verified
    PrepareStatement(BLOG_UPD_USER_BY_ADMIN, "UPDATE users SET username = ?, display_name = ?, email = ?, "
        "email_verified_at = CASE WHEN ?::boolean THEN COALESCE(email_verified_at, utc_now()) END, locale = ?, role = ?, denied = ? "
        "WHERE id = ? AND role <> 'admin' AND deleted_at IS NULL");
    // Deleted, keeping the comments: nothing of the person stays (the caller removes the picture's file,
    // the logins, sessions, mailed links and subscription)
    PrepareStatement(BLOG_UPD_USER_ANONYMIZE, "UPDATE users SET username = CONCAT('deleted_', id), display_name = '', email = CONCAT('deleted-', id, '@invalid'), "
        "email_verified_at = NULL, password_hash = NULL, role = 'user', denied = '', granted = '', drive_quota = NULL, drive_trash_days = NULL, totp_secret = NULL, totp_pending_secret = NULL, totp_last_step = NULL, "
        "banned_until = NULL, ban_reason = NULL, avatar = NULL, deleted_at = utc_now() WHERE id = ? AND role <> 'admin'");
    PrepareStatement(BLOG_DEL_USER_IDENTITIES, "DELETE FROM user_identity WHERE user_id = ?");
    PrepareStatement(BLOG_DEL_USER_ALL_EMAIL_TOKENS, "DELETE FROM email_token WHERE user_id = ?");
    PrepareStatement(BLOG_DEL_SUBSCRIBER_BY_EMAIL, "DELETE FROM subscriber WHERE email = ?");
    PrepareStatement(BLOG_SEL_USER_SESSIONS, "SELECT id_hash, ip, user_agent, created_at, last_seen_at, expires_at FROM auth_session "
        "WHERE user_id = ? AND expires_at > utc_now() ORDER BY last_seen_at DESC");
    PrepareStatement(BLOG_DEL_USER_SESSION, "DELETE FROM auth_session WHERE id_hash = ? AND user_id = ?");
    // Presence.h, once a minute per user online
    PrepareStatement(BLOG_UPD_USER_ONLINE, "UPDATE users SET online_seconds = online_seconds + ?, last_seen_at = utc_now() WHERE id = ?");
    // The admin can't be banned
    PrepareStatement(BLOG_UPD_USER_BAN, "UPDATE users SET banned_until = ?, ban_reason = ? WHERE id = ? AND role <> 'admin'");
    PrepareStatement(BLOG_SEL_IP_BANS, "SELECT ip, reason, created_at, expires_at FROM ip_ban WHERE expires_at IS NULL OR expires_at > utc_now() "
        "ORDER BY created_at DESC, ip");
    PrepareStatement(BLOG_REP_IP_BAN, "INSERT INTO ip_ban (ip, reason, expires_at) VALUES (?, ?, ?) ON CONFLICT (ip) DO UPDATE SET reason = EXCLUDED.reason, expires_at = EXCLUDED.expires_at, created_at = utc_now()");
    PrepareStatement(BLOG_DEL_IP_BAN, "DELETE FROM ip_ban WHERE ip = ?");

    // search: the posts whose translations match best, shown in the requested language. The query is
    // What the search index (SearchIndex.cpp) is built from: a row per language version of everything
    // published. One statement each, so each reads one moment of the tables
    PrepareStatement(BLOG_SEL_SEARCH_POSTS, "SELECT p.id, p.slug, p.cover, p.published_at, p.updated_at, p.original_lang, "
        "t.lang, t.title, t.summary, t.body_md, " BLOG_POST_TAGS " "
        "FROM post p JOIN post_translation t ON t.post_id = p.id WHERE p.status = 'published' ORDER BY p.id, t.lang DESC");
    PrepareStatement(BLOG_SEL_SEARCH_NOTES, "SELECT n.id, n.original_lang, n.published_at, t.lang, t.title, LEFT(t.body_md, 300), t.body_md "
        "FROM note n JOIN note_translation t ON t.note_id = n.id WHERE n.status = 'published' ORDER BY n.id, t.lang DESC");
    PrepareStatement(BLOG_SEL_SEARCH_TAGS, "SELECT slug, name_zh, name_en FROM tag");
    // Visible comments per post, for the search's results (BlogWorld keeps them)
    PrepareStatement(BLOG_SEL_POST_COMMENT_COUNTS, "SELECT target_id, COUNT(*) FROM comment WHERE target_kind = 'post' AND status = 'visible' "
        "GROUP BY target_id");
    PrepareStatement(BLOG_INS_UPLOAD, "INSERT INTO upload (user_id, file_name, mime, size, thumbhash, width, height, post_id, uploaded_by) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    PrepareStatement(BLOG_SEL_UPLOAD_META, "SELECT file_name, thumbhash, width, height FROM upload WHERE width IS NOT NULL");

    // drives
#define BLOG_DRIVE_FOLDER_COLUMNS "id, parent_id, name, created_at, trashed_at"
#define BLOG_DRIVE_FILE_COLUMNS "id, folder_id, name, size, mime, created_at, updated_at, stored_name, trashed_at, thumb"
    // Its files and the uploads on their way
    PrepareStatement(BLOG_SEL_DRIVE_USAGE, "SELECT CAST((SELECT COALESCE(SUM(size), 0) FROM drive_file WHERE user_id = ?) "
        "+ (SELECT COALESCE(SUM(size), 0) FROM drive_upload WHERE user_id = ?) "
        "+ (SELECT COALESCE(SUM(size), 0) FROM upload WHERE user_id = ? AND post_id IS NOT NULL) AS bigint), "
        "(SELECT COUNT(*) FROM drive_file WHERE user_id = ?)");
    PrepareStatement(BLOG_SEL_DRIVE_FOLDER, "SELECT " BLOG_DRIVE_FOLDER_COLUMNS " FROM drive_folder WHERE id = ? AND user_id = ?");
    PrepareStatement(BLOG_SEL_DRIVE_FOLDER_BY_NAME, "SELECT " BLOG_DRIVE_FOLDER_COLUMNS " FROM drive_folder WHERE user_id = ? AND parent_key = ? AND name = ? AND trash_key = 0");
    // In a folder (0: the root), not in the trash
    PrepareStatement(BLOG_SEL_DRIVE_SUBFOLDERS, "SELECT " BLOG_DRIVE_FOLDER_COLUMNS " FROM drive_folder WHERE user_id = ? AND parent_key = ? AND trashed_at IS NULL ORDER BY name");
    PrepareStatement(BLOG_SEL_DRIVE_ALL_FOLDERS, "SELECT " BLOG_DRIVE_FOLDER_COLUMNS " FROM drive_folder WHERE user_id = ? ORDER BY name");
    PrepareStatement(BLOG_INS_DRIVE_FOLDER, "INSERT INTO drive_folder (user_id, parent_id, name) VALUES (?, ?, ?)");
    PrepareStatement(BLOG_UPD_DRIVE_FOLDER, "UPDATE drive_folder SET parent_id = ?, name = ? WHERE id = ? AND user_id = ?");
    // With the folders under it (ON DELETE CASCADE); their files, uploads and links go first
    // (BLOG_DEL_DRIVE_*_IN_FOLDERS): the caller needs their stored names anyway
    PrepareStatement(BLOG_DEL_DRIVE_FOLDER, "DELETE FROM drive_folder WHERE id = ? AND user_id = ?");
    // A folder or a file of that name in a folder (0: the root), but in the trash
    PrepareStatement(BLOG_SEL_DRIVE_NAME_TAKEN, "SELECT EXISTS(SELECT 1 FROM drive_folder WHERE user_id = ? AND parent_key = ? AND name = ? AND trash_key = 0) "
        "OR EXISTS(SELECT 1 FROM drive_file WHERE user_id = ? AND folder_key = ? AND name = ? AND trash_key = 0)");
    PrepareStatement(BLOG_SEL_DRIVE_FILES, "SELECT " BLOG_DRIVE_FILE_COLUMNS " FROM drive_file WHERE user_id = ? AND folder_key = ? AND trashed_at IS NULL ORDER BY name");
    // Every file not in the trash itself (in a folder that is, maybe)
    PrepareStatement(BLOG_SEL_DRIVE_LIVE_FILES, "SELECT " BLOG_DRIVE_FILE_COLUMNS " FROM drive_file WHERE user_id = ? AND trashed_at IS NULL");
    PrepareStatement(BLOG_SEL_DRIVE_FILE, "SELECT " BLOG_DRIVE_FILE_COLUMNS " FROM drive_file WHERE id = ? AND user_id = ?");
    PrepareStatement(BLOG_SEL_DRIVE_FILE_BY_STORED_NAME, "SELECT " BLOG_DRIVE_FILE_COLUMNS " FROM drive_file WHERE stored_name = ?");
    // The files in a list of folders (FIND_IN_SET), and the uploads into them
    PrepareStatement(BLOG_SEL_DRIVE_STORED_NAMES_IN_FOLDERS, "SELECT stored_name FROM drive_file WHERE user_id = ? AND folder_id::text = ANY(string_to_array(?, ',')) "
        "UNION ALL SELECT CONCAT('upload:', id) FROM drive_upload WHERE user_id = ? AND folder_id::text = ANY(string_to_array(?, ','))");
    PrepareStatement(BLOG_DEL_DRIVE_SHARES_IN_FOLDERS, "DELETE FROM drive_share s USING drive_share x LEFT JOIN drive_file f ON f.id = x.file_id "
        "WHERE x.id = s.id AND s.user_id = ? AND (s.folder_id::text = ANY(string_to_array(?, ',')) OR f.folder_id::text = ANY(string_to_array(?, ',')))");
    PrepareStatement(BLOG_DEL_DRIVE_FILES_IN_FOLDERS, "DELETE FROM drive_file WHERE user_id = ? AND folder_id::text = ANY(string_to_array(?, ','))");
    PrepareStatement(BLOG_DEL_DRIVE_UPLOADS_IN_FOLDERS, "DELETE FROM drive_upload WHERE user_id = ? AND folder_id::text = ANY(string_to_array(?, ','))");
    PrepareStatement(BLOG_SEL_DRIVE_USER_STORED_NAMES, "SELECT stored_name FROM drive_file WHERE user_id = ?");
    PrepareStatement(BLOG_INS_DRIVE_FILE, "INSERT INTO drive_file (user_id, folder_id, name, stored_name, size, mime) VALUES (?, ?, ?, ?, ?, ?)");
    PrepareStatement(BLOG_UPD_DRIVE_FILE, "UPDATE drive_file SET folder_id = ?, name = ?, mime = ?, updated_at = utc_now() WHERE id = ? AND user_id = ?");
    PrepareStatement(BLOG_UPD_DRIVE_FILE_THUMB, "UPDATE drive_file SET thumb = ? WHERE id = ? AND user_id = ?");
    PrepareStatement(BLOG_DEL_DRIVE_FILE, "DELETE FROM drive_file WHERE id = ? AND user_id = ?");
    PrepareStatement(BLOG_DEL_DRIVE_USER_FILES, "DELETE FROM drive_file WHERE user_id = ?");
    PrepareStatement(BLOG_DEL_DRIVE_USER_FOLDERS, "DELETE FROM drive_folder WHERE user_id = ?");
    PrepareStatement(BLOG_INS_DRIVE_UPLOAD, "INSERT INTO drive_upload (id, user_id, folder_id, name, size) VALUES (?, ?, ?, ?, ?)");
    PrepareStatement(BLOG_SEL_DRIVE_UPLOAD, "SELECT folder_id, name, size, received FROM drive_upload WHERE id = ? AND user_id = ?");
    PrepareStatement(BLOG_UPD_DRIVE_UPLOAD_RECEIVED, "UPDATE drive_upload SET received = ? WHERE id = ? AND user_id = ?");
    PrepareStatement(BLOG_DEL_DRIVE_UPLOAD, "DELETE FROM drive_upload WHERE id = ?");
    PrepareStatement(BLOG_SEL_DRIVE_USER_UPLOADS, "SELECT id FROM drive_upload WHERE user_id = ?");
    PrepareStatement(BLOG_DEL_DRIVE_USER_UPLOADS, "DELETE FROM drive_upload WHERE user_id = ?");
    // Uploads left unfinished for a day
    PrepareStatement(BLOG_SEL_DRIVE_STALE_UPLOADS, "SELECT id FROM drive_upload WHERE created_at < utc_now() - INTERVAL '1 day'");
    // The trash: trash_key frees the name, a restore takes it back (in a folder and by a name not taken)
    PrepareStatement(BLOG_UPD_DRIVE_FILE_TRASH, "UPDATE drive_file SET trashed_at = utc_now(), trash_key = id WHERE id = ? AND user_id = ? AND trashed_at IS NULL");
    PrepareStatement(BLOG_UPD_DRIVE_FOLDER_TRASH, "UPDATE drive_folder SET trashed_at = utc_now(), trash_key = id WHERE id = ? AND user_id = ? AND trashed_at IS NULL");
    PrepareStatement(BLOG_UPD_DRIVE_FILE_RESTORE, "UPDATE drive_file SET trashed_at = NULL, trash_key = 0, folder_id = ?, name = ?, mime = ? WHERE id = ? AND user_id = ?");
    PrepareStatement(BLOG_UPD_DRIVE_FOLDER_RESTORE, "UPDATE drive_folder SET trashed_at = NULL, trash_key = 0, parent_id = ?, name = ? WHERE id = ? AND user_id = ?");
    PrepareStatement(BLOG_SEL_DRIVE_TRASHED_FILES, "SELECT " BLOG_DRIVE_FILE_COLUMNS " FROM drive_file WHERE user_id = ? AND trashed_at IS NOT NULL");
    PrepareStatement(BLOG_SEL_DRIVE_FOLDER_SIZES, "SELECT folder_id, CAST(SUM(size) AS bigint) FROM drive_file WHERE user_id = ? AND folder_id IS NOT NULL GROUP BY folder_id");
    // What was in the trash for its account's days (the 2nd ?: the default), or everything (the 1st ?),
    // of every account (3rd ? 0) or one (4th ?)
#define BLOG_DRIVE_EXPIRED(table, alias) "SELECT " alias ".user_id, " alias ".id FROM " table " " alias " JOIN users u ON u.id = " alias ".user_id " \
    "WHERE " alias ".trashed_at IS NOT NULL AND (? = 1 OR " alias ".trashed_at <= utc_now() - make_interval(days => COALESCE(u.drive_trash_days, ?::integer))) " \
    "AND (? = 0 OR " alias ".user_id = ?)"
    PrepareStatement(BLOG_SEL_DRIVE_EXPIRED_TRASH, "SELECT 'folder', x.* FROM (" BLOG_DRIVE_EXPIRED("drive_folder", "d") ") x "
        "UNION ALL SELECT 'file', y.* FROM (" BLOG_DRIVE_EXPIRED("drive_file", "f") ") y");
    // The console and the admin's page: accounts with a drive, or files from when they had one
    PrepareStatement(BLOG_SEL_DRIVE_ACCOUNTS, "SELECT u.id, u.username, u.role, u.granted, u.drive_quota, "
        "CAST(COALESCE(SUM(f.size), 0) AS bigint), COUNT(f.id), u.display_name, u.drive_trash_days, "
        "CAST(COALESCE(SUM(CASE WHEN f.trashed_at IS NULL THEN 0 ELSE f.size END), 0) AS bigint) FROM users u LEFT JOIN drive_file f ON f.user_id = u.id "
        "WHERE u.role = 'admin' OR 'drive' = ANY(string_to_array(u.granted, ',')) OR f.id IS NOT NULL GROUP BY u.id ORDER BY u.id");
    PrepareStatement(BLOG_UPD_USER_DRIVE, "UPDATE users SET granted = ?, drive_quota = ?, drive_trash_days = ? WHERE id = ? AND deleted_at IS NULL");
    PrepareStatement(BLOG_INS_DRIVE_SHARE, "INSERT INTO drive_share (token, user_id, file_id, folder_id, password_hash, expires_at) VALUES (?, ?, ?, ?, ?, ?)");
    // The owner's links: what they show, who opened them how often
#define BLOG_DRIVE_SHARE_COLUMNS "s.id, s.token, s.file_id, s.folder_id, COALESCE(f.name, d.name), s.password_hash IS NOT NULL, s.expires_at, s.created_at, " \
    "(SELECT COUNT(*) FROM drive_share_access a WHERE a.share_id = s.id), " \
    "(SELECT CAST(COALESCE(SUM(a.views), 0) AS bigint) FROM drive_share_access a WHERE a.share_id = s.id), " \
    "(SELECT CAST(COALESCE(SUM(a.downloads), 0) AS bigint) FROM drive_share_access a WHERE a.share_id = s.id), f.folder_id AS file_folder_id, f.trashed_at IS NOT NULL"
    PrepareStatement(BLOG_SEL_DRIVE_SHARES, "SELECT " BLOG_DRIVE_SHARE_COLUMNS " FROM drive_share s LEFT JOIN drive_file f ON f.id = s.file_id "
        "LEFT JOIN drive_folder d ON d.id = s.folder_id WHERE s.user_id = ? AND (? = 0 OR s.id = ?) ORDER BY s.id DESC");
    PrepareStatement(BLOG_SEL_DRIVE_SHARE_ACCESS, "SELECT u.id, u.display_name, u.username, a.views, a.downloads, a.first_at, a.last_at "
        "FROM drive_share_access a JOIN drive_share s ON s.id = a.share_id JOIN users u ON u.id = a.user_id "
        "WHERE a.share_id = ? AND s.user_id = ? ORDER BY a.last_at DESC");
    PrepareStatement(BLOG_DEL_DRIVE_SHARE, "DELETE FROM drive_share WHERE id = ? AND user_id = ?");
    // A link as someone opens it (the 1st ?): whether it ran out, whose it is, whether they gave its password
    PrepareStatement(BLOG_SEL_DRIVE_SHARE_BY_TOKEN, "SELECT s.id, s.user_id, s.file_id, s.folder_id, s.password_hash, s.expires_at, "
        "s.expires_at IS NOT NULL AND s.expires_at <= utc_now(), u.display_name, u.role, u.granted, u.deleted_at IS NOT NULL, COALESCE(a.unlocked, false) "
        "FROM drive_share s JOIN users u ON u.id = s.user_id LEFT JOIN drive_share_access a ON a.share_id = s.id AND a.user_id = ? WHERE s.token = ?");
    PrepareStatement(BLOG_INS_DRIVE_SHARE_VIEW, "INSERT INTO drive_share_access (share_id, user_id, views) VALUES (?, ?, 1) "
        "ON CONFLICT (share_id, user_id) DO UPDATE SET views = drive_share_access.views + 1, last_at = utc_now()");
    PrepareStatement(BLOG_INS_DRIVE_SHARE_DOWNLOAD, "INSERT INTO drive_share_access (share_id, user_id, downloads) VALUES (?, ?, 1) "
        "ON CONFLICT (share_id, user_id) DO UPDATE SET downloads = drive_share_access.downloads + 1, last_at = utc_now()");
    PrepareStatement(BLOG_INS_DRIVE_SHARE_UNLOCK, "INSERT INTO drive_share_access (share_id, user_id, unlocked) VALUES (?, ?, true) "
        "ON CONFLICT (share_id, user_id) DO UPDATE SET unlocked = true, last_at = utc_now()");

    // Reads of the site (Analytics.h): sums added to the day's
    PrepareStatement(BLOG_INS_PAGE_VIEWS, "INSERT INTO page_view (day, path, views, visitors) VALUES (?, ?, ?, ?) "
        "ON CONFLICT (day, path) DO UPDATE SET views = page_view.views + EXCLUDED.views, visitors = page_view.visitors + EXCLUDED.visitors");
    PrepareStatement(BLOG_INS_PAGE_REFERRER, "INSERT INTO page_referrer (day, host, views) VALUES (?, ?, ?) "
        "ON CONFLICT (day, host) DO UPDATE SET views = page_referrer.views + EXCLUDED.views");
    // Since a day (the 1st ?)
    PrepareStatement(BLOG_SEL_STATS_DAYS, "SELECT to_char(day, 'YYYY-MM-DD'), views, visitors FROM page_view WHERE path = '' AND day >= ? ORDER BY day");
    PrepareStatement(BLOG_SEL_STATS_PAGES, "SELECT path, CAST(SUM(views) AS bigint), CAST(SUM(visitors) AS bigint) FROM page_view "
        "WHERE path <> '' AND day >= ? GROUP BY path ORDER BY SUM(views) DESC, path LIMIT 50");
    PrepareStatement(BLOG_SEL_STATS_REFERRERS, "SELECT host, CAST(SUM(views) AS bigint) FROM page_referrer WHERE day >= ? "
        "GROUP BY host ORDER BY SUM(views) DESC, host LIMIT 30");

    PrepareStatement(BLOG_SEL_MIGRATIONS, "SELECT version FROM schema_migrations");

    // Who may do what to a post: its owner, and the user's editor row if it has one
    PrepareStatement(BLOG_SEL_POST_ACCESS, "SELECT p.author_id, e.user_id IS NOT NULL, e.rights FROM post p "
        "LEFT JOIN post_editor e ON e.post_id = p.id AND e.user_id = ? WHERE p.id = ?");
    // A writer's posts: its own and the ones it edits, as BLOG_SEL_ADMIN_POSTS lists them
    PrepareStatement(BLOG_SEL_EDITOR_POSTS, "SELECT p.id, p.slug, p.status, p.original_lang, p.updated_at, p.published_at, COALESCE(od.title, o.title), " BLOG_POST_LANGS ", p.publish_at, p.author_id, p.review_requested_at IS NOT NULL "
        "FROM post p JOIN post_translation o ON o.post_id = p.id AND o.lang = p.original_lang "
        "LEFT JOIN post_draft od ON od.post_id = p.id AND od.lang = p.original_lang "
        "WHERE (p.author_id = ? OR EXISTS (SELECT 1 FROM post_editor e WHERE e.post_id = p.id AND e.user_id = ?)) "
        "AND (? = '' OR p.status = ? OR (? = 'review' AND p.review_requested_at IS NOT NULL)) ORDER BY p.updated_at DESC, p.id DESC LIMIT ? OFFSET ?");
    PrepareStatement(BLOG_SEL_POST_EDITORS, "SELECT u.id, u.username, u.display_name, u.avatar, e.rights, e.created_at FROM post_editor e "
        "JOIN users u ON u.id = e.user_id WHERE e.post_id = ? AND u.deleted_at IS NULL ORDER BY e.created_at, u.id");
    PrepareStatement(BLOG_REP_POST_EDITOR, "INSERT INTO post_editor (post_id, user_id, rights, invited_by) VALUES (?, ?, ?, ?) "
        "ON CONFLICT (post_id, user_id) DO UPDATE SET rights = EXCLUDED.rights");
    PrepareStatement(BLOG_DEL_POST_EDITOR, "DELETE FROM post_editor WHERE post_id = ? AND user_id = ?");
    PrepareStatement(BLOG_SEL_ACTIVE_USER_BY_NAME, "SELECT id FROM users WHERE username = ? AND deleted_at IS NULL");
    PrepareStatement(BLOG_UPD_POST_OWNER, "UPDATE post SET author_id = ? WHERE id = ?");
    PrepareStatement(BLOG_UPD_USER_GRANTED, "UPDATE users SET granted = ? WHERE id = ? AND deleted_at IS NULL");

    // A post's working copy (Collab.h): each field of a language as its editors left it, or as
    // published, with its document's version; a write of an older version leaves a newer one
    PrepareStatement(BLOG_SEL_POST_DOC_TEXTS, "SELECT COALESCE(d.title, t.title), d.title_v, COALESCE(d.summary, t.summary), d.summary_v, "
        "COALESCE(d.body_md, t.body_md), d.body_v FROM post p LEFT JOIN post_translation t ON t.post_id = p.id AND t.lang = ? "
        "LEFT JOIN post_draft d ON d.post_id = p.id AND d.lang = ? WHERE p.id = ?");
    PrepareStatement(BLOG_UPS_POST_DRAFT_TITLE, "INSERT INTO post_draft (post_id, lang, title, title_v) VALUES (?, ?, ?, ?) "
        "ON CONFLICT (post_id, lang) DO UPDATE SET title = CASE WHEN EXCLUDED.title_v > post_draft.title_v THEN EXCLUDED.title ELSE post_draft.title END, "
        "title_v = GREATEST(post_draft.title_v, EXCLUDED.title_v), updated_at = utc_now()");
    PrepareStatement(BLOG_UPS_POST_DRAFT_SUMMARY, "INSERT INTO post_draft (post_id, lang, summary, summary_v) VALUES (?, ?, ?, ?) "
        "ON CONFLICT (post_id, lang) DO UPDATE SET summary = CASE WHEN EXCLUDED.summary_v > post_draft.summary_v THEN EXCLUDED.summary ELSE post_draft.summary END, "
        "summary_v = GREATEST(post_draft.summary_v, EXCLUDED.summary_v), updated_at = utc_now()");
    PrepareStatement(BLOG_UPS_POST_DRAFT_BODY, "INSERT INTO post_draft (post_id, lang, body_md, body_v) VALUES (?, ?, ?, ?) "
        "ON CONFLICT (post_id, lang) DO UPDATE SET body_md = CASE WHEN EXCLUDED.body_v > post_draft.body_v THEN EXCLUDED.body_md ELSE post_draft.body_md END, "
        "body_v = GREATEST(post_draft.body_v, EXCLUDED.body_v), updated_at = utc_now()");
    // What publishing copies: each language's working copy, its fields not changed as published
    PrepareStatement(BLOG_SEL_POST_DRAFTS, "SELECT d.lang, COALESCE(d.title, t.title), COALESCE(d.summary, t.summary, ''), COALESCE(d.body_md, t.body_md) "
        "FROM post_draft d LEFT JOIN post_translation t ON t.post_id = d.post_id AND t.lang = d.lang WHERE d.post_id = ? ORDER BY d.lang DESC");
    PrepareStatement(BLOG_UPD_POST_PUBLISH, "UPDATE post SET status = 'published', published_at = COALESCE(published_at, utc_now()), "
        "publish_at = NULL, review_requested_at = NULL, review_requested_by = NULL, updated_at = utc_now() WHERE id = ?");
    // Someone's page (PersonGet): the account, the published posts it owns or edits in a language
    PrepareStatement(BLOG_SEL_PERSON, "SELECT id, username, display_name, avatar, created_at FROM users WHERE username = ? AND deleted_at IS NULL");
    PrepareStatement(BLOG_SEL_PERSON_POSTS, "SELECT p.id, p.slug, p.cover, p.published_at, p.updated_at, p.original_lang, "
        "t.lang, t.title, t.summary, " BLOG_POST_TAGS ", " BLOG_POST_LANGS ", " BLOG_POST_COMMENT_COUNT " "
        BLOG_POST_TEXT_JOINS "WHERE p.status = 'published' "
        "AND (p.author_id = ? OR EXISTS (SELECT 1 FROM post_editor e WHERE e.post_id = p.id AND e.user_id = ?)) "
        "ORDER BY p.published_at DESC, p.id DESC LIMIT 100");
    // Who wrote a post: its owner, then its editors as they were invited
    PrepareStatement(BLOG_SEL_POST_PEOPLE, "SELECT u.id, u.username, u.display_name, u.avatar, 0 AS place, p.created_at AS since FROM post p "
        "JOIN users u ON u.id = p.author_id WHERE p.id = ? UNION ALL SELECT u.id, u.username, u.display_name, u.avatar, 1, e.created_at "
        "FROM post_editor e JOIN users u ON u.id = e.user_id WHERE e.post_id = ? AND u.deleted_at IS NULL ORDER BY place, since");
    // Asked to be published (NULL: not, or not any more)
    PrepareStatement(BLOG_UPD_POST_REVIEW, "UPDATE post SET review_requested_at = ?, review_requested_by = ? WHERE id = ?");
    PrepareStatement(BLOG_SEL_POST_DRAFT_LANGS, "SELECT lang FROM post_draft WHERE post_id = ? AND (title IS NOT NULL OR body_md IS NOT NULL) "
        "ORDER BY lang DESC");
    PrepareStatement(BLOG_DEL_POST_DRAFT_LANG, "DELETE FROM post_draft WHERE post_id = ? AND lang = ?");

    // User friendships and requests
    PrepareStatement(BLOG_SEL_USER_FRIENDS, "SELECT u.id, u.username, u.display_name, u.avatar, uf.created_at "
        "FROM user_friend uf JOIN users u ON u.id = uf.friend_id "
        "WHERE uf.user_id = ? AND u.deleted_at IS NULL AND (u.banned_until IS NULL OR u.banned_until < utc_now()) "
        "ORDER BY u.display_name ASC, u.id ASC");
    PrepareStatement(BLOG_SEL_USER_FRIEND_REQUESTS_IN, "SELECT r.id, u.id AS user_id, u.username, u.display_name, u.avatar, r.message, r.created_at "
        "FROM user_friend_request r JOIN users u ON u.id = r.from_user_id "
        "WHERE r.to_user_id = ? AND u.deleted_at IS NULL AND (u.banned_until IS NULL OR u.banned_until < utc_now()) "
        "ORDER BY r.id DESC");
    PrepareStatement(BLOG_SEL_USER_FRIEND_REQUESTS_OUT, "SELECT r.id, u.id AS user_id, u.username, u.display_name, u.avatar, r.message, r.created_at "
        "FROM user_friend_request r JOIN users u ON u.id = r.to_user_id "
        "WHERE r.from_user_id = ? AND u.deleted_at IS NULL AND (u.banned_until IS NULL OR u.banned_until < utc_now()) "
        "ORDER BY r.id DESC");
    PrepareStatement(BLOG_SEL_USER_FRIEND_REQUEST_BY_ID, "SELECT id, from_user_id, to_user_id, message, created_at "
        "FROM user_friend_request WHERE id = ?");
    PrepareStatement(BLOG_SEL_USER_FRIEND_PAIR, "SELECT created_at FROM user_friend WHERE user_id = ? AND friend_id = ?");
    PrepareStatement(BLOG_SEL_USER_FRIEND_REQUEST_PAIR, "SELECT id, from_user_id, to_user_id FROM user_friend_request "
        "WHERE (from_user_id = ? AND to_user_id = ?) OR (from_user_id = ? AND to_user_id = ?)");
    PrepareStatement(BLOG_INS_USER_FRIEND, "INSERT INTO user_friend (user_id, friend_id, created_at) VALUES (?, ?, utc_now()) ON CONFLICT DO NOTHING");
    PrepareStatement(BLOG_DEL_USER_FRIEND, "DELETE FROM user_friend WHERE (user_id = ? AND friend_id = ?) OR (user_id = ? AND friend_id = ?)");
    PrepareStatement(BLOG_INS_USER_FRIEND_REQUEST, "INSERT INTO user_friend_request (from_user_id, to_user_id, message, created_at) "
        "VALUES (?, ?, ?, utc_now())");
    PrepareStatement(BLOG_DEL_USER_FRIEND_REQUEST, "DELETE FROM user_friend_request WHERE id = ?");
    PrepareStatement(BLOG_DEL_USER_FRIEND_REQUEST_PAIR, "DELETE FROM user_friend_request "
        "WHERE (from_user_id = ? AND to_user_id = ?) OR (from_user_id = ? AND to_user_id = ?)");
    PrepareStatement(BLOG_SEL_USER_FOR_FRIEND_BY_NAME, "SELECT id, username, display_name, avatar FROM users "
        "WHERE username = ? AND deleted_at IS NULL AND (banned_until IS NULL OR banned_until < utc_now())");
    PrepareStatement(BLOG_SEL_USER_FOR_FRIEND_BY_ID, "SELECT id, username, display_name, avatar FROM users "
        "WHERE id = ? AND deleted_at IS NULL AND (banned_until IS NULL OR banned_until < utc_now())");

    // Chat
    PrepareStatement(BLOG_SEL_CHAT_MESSAGES_INITIAL, "SELECT m.id, m.channel_id, m.sender_id, u.display_name, u.avatar, m.content, m.created_at "
        "FROM chat_message m JOIN users u ON u.id = m.sender_id "
        "WHERE m.channel_id = ? ORDER BY m.id DESC LIMIT ?");
    PrepareStatement(BLOG_SEL_CHAT_MESSAGES_BEFORE, "SELECT m.id, m.channel_id, m.sender_id, u.display_name, u.avatar, m.content, m.created_at "
        "FROM chat_message m JOIN users u ON u.id = m.sender_id "
        "WHERE m.channel_id = ? AND m.id < ? ORDER BY m.id DESC LIMIT ?");
    PrepareStatement(BLOG_INS_CHAT_MESSAGE, "INSERT INTO chat_message (channel_id, sender_id, content, created_at) "
        "VALUES (?, ?, ?, utc_now())");
    PrepareStatement(BLOG_SEL_CHAT_MESSAGE, "SELECT m.id, m.channel_id, m.sender_id, u.display_name, u.avatar, m.content, m.created_at "
        "FROM chat_message m JOIN users u ON u.id = m.sender_id WHERE m.id = ?");
    PrepareStatement(BLOG_SEL_CHAT_LAST_MESSAGE, "SELECT m.id, m.channel_id, m.sender_id, u.display_name, u.avatar, m.content, m.created_at "
        "FROM chat_message m JOIN users u ON u.id = m.sender_id "
        "WHERE m.channel_id = ? ORDER BY m.id DESC LIMIT 1");
    PrepareStatement(BLOG_UPS_CHAT_READ, "INSERT INTO chat_read (user_id, channel_id, last_read_id, read_at) "
        "VALUES (?, ?, LEAST(?, (SELECT COALESCE(MAX(id), 0) FROM chat_message WHERE channel_id = ?)), utc_now()) "
        "ON CONFLICT (user_id, channel_id) DO UPDATE SET last_read_id = GREATEST(chat_read.last_read_id, EXCLUDED.last_read_id), read_at = utc_now()");
    PrepareStatement(BLOG_SEL_CHAT_UNREAD, "SELECT COUNT(*) FROM chat_message "
        "WHERE channel_id = ? AND id > COALESCE((SELECT last_read_id FROM chat_read WHERE user_id = ? AND channel_id = ?), 0)");
    PrepareStatement(BLOG_SEL_CHAT_THRESHOLD_FIFO, "SELECT id FROM chat_message WHERE channel_id = ? ORDER BY id DESC LIMIT 1 OFFSET ?");
    PrepareStatement(BLOG_DEL_CHAT_FIFO, "DELETE FROM chat_message WHERE channel_id = ? AND id <= ?");
    PrepareStatement(BLOG_SEL_CHAT_STATS, "SELECT COUNT(*), COUNT(DISTINCT channel_id) FROM chat_message");

    // Chat groups
    PrepareStatement(BLOG_INS_CHAT_GROUP, "INSERT INTO chat_group (name, owner_id, created_at) VALUES (?, ?, utc_now())");
    PrepareStatement(BLOG_INS_CHAT_GROUP_MEMBER, "INSERT INTO chat_group_member (group_id, user_id, role, joined_at) "
        "VALUES (?, ?, ?, utc_now()) ON CONFLICT DO NOTHING");
    PrepareStatement(BLOG_SEL_USER_CHAT_GROUPS, "SELECT g.id, g.name, g.owner_id, "
        "(SELECT COUNT(*) FROM chat_group_member WHERE group_id = g.id) AS member_count "
        "FROM chat_group g JOIN chat_group_member m ON m.group_id = g.id "
        "WHERE m.user_id = ? ORDER BY g.id DESC");
    PrepareStatement(BLOG_SEL_CHAT_GROUP_MEMBERS, "SELECT u.id, u.username, u.display_name, u.avatar, m.role "
        "FROM chat_group_member m JOIN users u ON u.id = m.user_id "
        "WHERE m.group_id = ? AND u.deleted_at IS NULL AND (u.banned_until IS NULL OR u.banned_until < utc_now()) "
        "ORDER BY (m.role = 'owner') DESC, u.display_name ASC");
    PrepareStatement(BLOG_SEL_CHAT_GROUP_MEMBER_CHECK, "SELECT role FROM chat_group_member WHERE group_id = ? AND user_id = ?");
    PrepareStatement(BLOG_DEL_CHAT_GROUP_MEMBER, "DELETE FROM chat_group_member WHERE group_id = ? AND user_id = ?");
    PrepareStatement(BLOG_DEL_CHAT_GROUP, "DELETE FROM chat_group WHERE id = ? AND owner_id = ?");
    PrepareStatement(BLOG_SEL_CHAT_GROUP_MEMBER_IDS, "SELECT user_id FROM chat_group_member WHERE group_id = ?");
    PrepareStatement(BLOG_SEL_CHAT_GROUP_MEMBER_COUNT, "SELECT COUNT(*) FROM chat_group_member WHERE group_id = ?");
    PrepareStatement(BLOG_DEL_CHAT_GROUP_BY_ID, "DELETE FROM chat_group WHERE id = ?");
    // A channel's messages and read cursors: a group dissolved, a channel cleared
    PrepareStatement(BLOG_DEL_CHAT_CHANNEL_MESSAGES, "DELETE FROM chat_message WHERE channel_id = ?");
    PrepareStatement(BLOG_DEL_CHAT_CHANNEL_READS, "DELETE FROM chat_read WHERE channel_id = ?");
    PrepareStatement(BLOG_DEL_CHAT_MESSAGE, "DELETE FROM chat_message WHERE id = ?");
    PrepareStatement(BLOG_SEL_ADMIN_CHAT_GROUPS, "SELECT g.id, g.name, g.owner_id, u.username, "
        "(SELECT COUNT(*) FROM chat_group_member WHERE group_id = g.id), "
        "(SELECT COUNT(*) FROM chat_message WHERE channel_id = CONCAT('group:', g.id)), g.created_at "
        "FROM chat_group g JOIN users u ON u.id = g.owner_id ORDER BY g.id DESC");
    // 51: the 51st says there is a next page
    PrepareStatement(BLOG_SEL_ADMIN_CHAT_MESSAGES, "SELECT m.id, m.channel_id, m.sender_id, u.username, u.display_name, m.content, m.created_at "
        "FROM chat_message m JOIN users u ON u.id = m.sender_id "
        "WHERE m.channel_id = ? AND m.id < ? ORDER BY m.id DESC LIMIT 51");
}

template class DatabaseWorkerPool<BlogDatabaseConnection>;

BlogDatabaseWorkerPool BlogDatabase;
