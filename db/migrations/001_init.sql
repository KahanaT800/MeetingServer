-- Meeting Server initial schema
DROP DATABASE IF EXISTS meeting;
CREATE DATABASE IF NOT EXISTS meeting CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

USE meeting;

-- Users table
CREATE TABLE IF NOT EXISTS users (
    id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    user_uuid CHAR(36) NOT NULL,
    username VARCHAR(64) NOT NULL,
    display_name VARCHAR(128) NOT NULL,
    email VARCHAR(128) NOT NULL,
    password_hash VARCHAR(255) NOT NULL,
    salt VARCHAR(64) NOT NULL,
    status TINYINT NOT NULL DEFAULT 1 COMMENT '1: active, 2: locked, 0: deleted ',
    password_version INT NOT NULL DEFAULT 1,
    last_login_ip VARCHAR(64) NULL,
    last_login_at DATETIME NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    deleted_at TIMESTAMP NULL,
    UNIQUE KEY uk_users_uuid (user_uuid),
    UNIQUE KEY uk_users_username (username),
    UNIQUE KEY uk_users_email (email),
    KEY idx_users_status (status)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;

-- Seesions table
CREATE TABLE IF NOT EXISTS user_sessions (
    id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    user_id BIGINT UNSIGNED NOT NULL,
    access_token CHAR(64) NOT NULL,
    refresh_token CHAR(64) NOT NULL,
    client_ip VARCHAR(64) NULL,
    user_agent VARCHAR(255) NULL,
    expires_at DATETIME NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    revoked_at TIMESTAMP NULL,
    UNIQUE KEY uk_user_sessions_access (access_token),
    UNIQUE KEY uk_user_sessions_refresh (refresh_token),
    KEY idx_user_sessions_user (user_id, expires_at),
    CONSTRAINT fk_user_sessions_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;

-- Meetings table
CREATE TABLE IF NOT EXISTS meetings (
    id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    meeting_id CHAR(26) NOT NULL COMMENT 'public snowflake/ksuid',
    meeting_code VARCHAR(16) NOT NULL,
    organizer_id BIGINT UNSIGNED NOT NULL,
    topic VARCHAR(128) NOT NULL,
    description TEXT NULL,
    state TINYINT NOT NULL DEFAULT 0 COMMENT '0: scheduled, 1: running, 2: ended',
    geo_region VARCHAR(32) NULL,
    server_endpoint VARCHAR(128) NULL,
    max_participants INT NOT NULL DEFAULT 100,
    statrt_time DATETIME NULL,
    end_time DATETIME NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    deleted_at TIMESTAMP NULL,
    UNIQUE KEY uk_meetings_meeting_id (meeting_id),
    UNIQUE KEY uk_meetings_meeting_code (meeting_code),
    KEY idx_meetings_state (state, statrt_time),
    KEY idx_meetings_organizer (organizer_id, state),
    CONSTRAINT fk_meetings_organizer FOREIGN KEY (organizer_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;

-- Participants table
CREATE TABLE IF NOT EXISTS meeting_participants (
    id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    meeting_id BIGINT UNSIGNED NOT NULL,
    user_id BIGINT UNSIGNED NOT NULL,
    role TINYINT NOT NULL DEFAULT 0 COMMENT '0 : participant, 1: host, 2: cohost',
    device_type VARCHAR(32) NULL,
    network_latency_ms INT NULL,
    joined_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    left_at DATETIME NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uk_participants_meeting_user (meeting_id, user_id),
    KEY idx_participants_user_role (user_id, role),
    CONSTRAINT fk_participants_meeting FOREIGN KEY (meeting_id) REFERENCES meetings(id) ON DELETE CASCADE,
    CONSTRAINT fk_participants_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_unicode_ci;

-- Meeting events / audit trail table
CREATE TABLE IF NOT EXISTS meeting_events (
    id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    meeting_id BIGINT UNSIGNED NOT NULL,
    event_type VARCHAR(32) NOT NULL,
    payload JSON NULL,
    created_by BIGINT UNSIGNED NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    KEY idx_meeting_events_meeting (meeting_id, created_at),
    CONSTRAINT fk_meeting_events_meeting FOREIGN KEY (meeting_id) REFERENCES meetings (id) ON DELETE CASCADE
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_unicode_ci;
