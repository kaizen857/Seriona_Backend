CREATE TABLE IF NOT EXISTS content(
  content_id TEXT PRIMARY KEY,
  title TEXT NOT NULL,
  artist TEXT NOT NULL,
  album TEXT NOT NULL,
  album_artist TEXT NOT NULL,
  genre TEXT NOT NULL,
  track_number INTEGER,
  disc_number INTEGER,
  year INTEGER,
  duration_ms INTEGER NOT NULL,
  sample_rate INTEGER,
  bit_depth INTEGER,
  channels INTEGER,
  play_count INTEGER NOT NULL DEFAULT 0,
  rating INTEGER,
  last_played_ms INTEGER,
  created_at_ms INTEGER NOT NULL,
  updated_at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS scan_roots(
  root_path TEXT PRIMARY KEY,
  directory_tree_hash TEXT NOT NULL,
  total_files INTEGER NOT NULL,
  last_scan_mode TEXT NOT NULL,
  last_scan_duration_ms INTEGER NOT NULL,
  last_scan_at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS locations(
  location_id TEXT PRIMARY KEY,
  content_id TEXT NOT NULL,
  root_path TEXT NOT NULL,
  file_path TEXT NOT NULL UNIQUE,
  file_size_bytes INTEGER NOT NULL,
  file_mtime_ns INTEGER NOT NULL,
  source_file_path TEXT NOT NULL,
  cue_track_offset_ms INTEGER,
  artwork_path TEXT,
  thumbnail_path TEXT,
  lyrics_source TEXT NOT NULL,
  external_lrc_path TEXT,
  external_lrc_mtime_ns INTEGER,
  discovered_at_ms INTEGER NOT NULL,
  scanned_at_ms INTEGER NOT NULL,
  FOREIGN KEY(content_id) REFERENCES content(content_id) ON DELETE CASCADE,
  FOREIGN KEY(root_path) REFERENCES scan_roots(root_path) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS lyrics(
  location_id TEXT NOT NULL,
  kind TEXT NOT NULL,
  line_index INTEGER NOT NULL,
  timestamp_ms INTEGER NOT NULL,
  text TEXT NOT NULL,
  PRIMARY KEY(location_id, kind, line_index),
  FOREIGN KEY(location_id) REFERENCES locations(location_id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS scan_errors(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  root_path TEXT NOT NULL,
  file_path TEXT,
  error_code TEXT NOT NULL,
  error_message TEXT NOT NULL,
  occurred_at_ms INTEGER NOT NULL,
  FOREIGN KEY(root_path) REFERENCES scan_roots(root_path) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_content_album ON content(album);
CREATE INDEX IF NOT EXISTS idx_content_artist ON content(artist);
CREATE INDEX IF NOT EXISTS idx_locations_content ON locations(content_id);
CREATE INDEX IF NOT EXISTS idx_locations_root ON locations(root_path);
CREATE INDEX IF NOT EXISTS idx_locations_path ON locations(file_path);
CREATE INDEX IF NOT EXISTS idx_lyrics_location ON lyrics(location_id);
CREATE INDEX IF NOT EXISTS idx_errors_root ON scan_errors(root_path);

PRAGMA user_version=3;
