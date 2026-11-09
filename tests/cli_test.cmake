if(NOT DEFINED CLI OR NOT DEFINED WORK)
  message(FATAL_ERROR "CLI and WORK required")
endif()
file(MAKE_DIRECTORY "${WORK}")
set(DB "${WORK}/cli.db")
# Only exact test-owned files are removed; never remove the directory recursively.
file(REMOVE "${DB}" "${DB}.wal" "${WORK}/other.db" "${WORK}/other.db.wal")
file(WRITE "${WORK}/fixture.sql" [=[
-- statement delimiters inside comments ; do not end statements
CREATE TABLE notes (
  id INTEGER PRIMARY KEY,
  body VARCHAR(80) NOT NULL
);
/* delimiters ; in a block comment */
INSERT INTO notes VALUES (1,'semi;colon'),
                         (2,'O''Brien');
SELECT body FROM notes ORDER BY id;
]=])
execute_process(COMMAND "${CLI}" --database "${DB}" --buffer-pages 2 --file "${WORK}/fixture.sql"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(NOT status EQUAL 0 OR NOT output MATCHES "semi;colon" OR NOT output MATCHES "O'Brien")
  message(FATAL_ERROR "SQL file / multiline / quotes failed: ${status}\n${output}\n${error}")
endif()
file(WRITE "${WORK}/input.txt" ".tables\n.schema\nBEGIN;\nINSERT INTO notes VALUES (3,'abandon');\n.exit\n")
execute_process(COMMAND "${CLI}" --database "${DB}" --batch INPUT_FILE "${WORK}/input.txt"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(NOT status EQUAL 0 OR NOT output MATCHES "notes")
  message(FATAL_ERROR "meta commands failed: ${status}\n${output}\n${error}")
endif()
execute_process(COMMAND "${CLI}" "${DB}" --execute "SELECT COUNT(*) AS total FROM notes; SELECT body FROM notes WHERE id=3;"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(NOT status EQUAL 0 OR NOT output MATCHES "0 row[(]s[)]" OR output MATCHES "abandon")
  message(FATAL_ERROR "CLI exit must roll back: ${status}\n${output}\n${error}")
endif()
execute_process(COMMAND "${CLI}" "${DB}" --execute "BEGIN; INSERT INTO notes VALUES(4,'abort'); INSERT INTO notes VALUES(1,'duplicate'); COMMIT;"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(status EQUAL 0 OR NOT error MATCHES "Error:")
  message(FATAL_ERROR "batch errors must return nonzero: ${status}\n${output}\n${error}")
endif()
execute_process(COMMAND "${CLI}" "${DB}" --execute "SELECT body FROM notes WHERE id=4;"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(NOT status EQUAL 0 OR NOT output MATCHES "0 row[(]s[)]")
  message(FATAL_ERROR "failed CLI transaction must roll back: ${status}\n${output}\n${error}")
endif()
file(WRITE "${WORK}/input.txt" ".open \"${WORK}/other.db\"\nCREATE TABLE second(id INTEGER);\n.tables\n.checkpoint\n.exit\n")
execute_process(COMMAND "${CLI}" --database "${DB}" --batch INPUT_FILE "${WORK}/input.txt"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(NOT status EQUAL 0 OR NOT output MATCHES "second")
  message(FATAL_ERROR "open/checkpoint meta commands failed: ${status}\n${output}\n${error}")
endif()
execute_process(COMMAND "${CLI}" --database "${DB}" --execute "SELECT 'unterminated"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(status EQUAL 0)
  message(FATAL_ERROR "unterminated SQL must fail")
endif()
message(STATUS "CLI integration passed: SQL file, multiline, quoted semicolon, meta commands, rollback and nonzero errors")
