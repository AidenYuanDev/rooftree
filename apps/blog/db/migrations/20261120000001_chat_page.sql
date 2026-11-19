-- migrate:up

-- The chat page: replies, files and pictures, edits and recalls, @mentions, pinned and muted chats

ALTER TABLE chat_message
  ADD COLUMN reply_to_id bigint REFERENCES chat_message (id) ON DELETE SET NULL,  -- quoted; gone when it is
  ADD COLUMN edited_at timestamp(0),
  ADD COLUMN recalled boolean NOT NULL DEFAULT false,      -- its sender took it back: no content, no file
  ADD COLUMN file_name varchar(255),                       -- an attached file, as its sender named it; NULL: none
  ADD COLUMN file_mime varchar(100),
  ADD COLUMN file_size bigint,
  ADD COLUMN file_stored varchar(64);                      -- its name under Blog.Uploads.Directory/chat
CREATE INDEX chat_message_reply ON chat_message (reply_to_id) WHERE reply_to_id IS NOT NULL;

-- Who a message @mentioned, for their unread mentions
CREATE TABLE chat_mention (
  message_id bigint NOT NULL REFERENCES chat_message (id) ON DELETE CASCADE,
  user_id bigint NOT NULL REFERENCES users (id) ON DELETE CASCADE,
  channel_id varchar(64) NOT NULL,
  PRIMARY KEY (message_id, user_id)
);
CREATE INDEX chat_mention_user ON chat_mention (user_id, channel_id, message_id);

-- An account's own settings of a chat
CREATE TABLE chat_channel_pref (
  user_id bigint NOT NULL REFERENCES users (id) ON DELETE CASCADE,
  channel_id varchar(64) NOT NULL,
  pinned_at timestamp(0),                                  -- NULL: not pinned
  muted boolean NOT NULL DEFAULT false,                    -- not counted in the unread total, mentions still are
  PRIMARY KEY (user_id, channel_id)
);

-- migrate:down

DROP TABLE chat_channel_pref;
DROP TABLE chat_mention;
DROP INDEX chat_message_reply;
ALTER TABLE chat_message
  DROP COLUMN reply_to_id,
  DROP COLUMN edited_at,
  DROP COLUMN recalled,
  DROP COLUMN file_name,
  DROP COLUMN file_mime,
  DROP COLUMN file_size,
  DROP COLUMN file_stored;
