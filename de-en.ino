#include <stdio.h>
#include <stdlib.h>
#include <sqlite3.h>
#include <unordered_map>
#include <SPI.h>
#include <FS.h>
#include "SD.h"
#include "M5Cardputer.h"
#include "M5GFX.h"

#define TEXTSIZE 2.6
#define DEBOUNCE 30

M5Canvas canvas(&M5Cardputer.Display);
String input_data = "> ";
sqlite3 *transdb;
sqlite3 *infodb;
int before = -DEBOUNCE;      // For debouncing
bool update_battery = false; // For keeping it always shown

const int MAX_QUERY_LENGTH = 256; // If it's larger, well…
const int NUM_QUERIES = 2;
char queries[NUM_QUERIES][MAX_QUERY_LENGTH];

const char *data = "Callback function called";
static int callbackTrans(void *data, int argc, char **argv, char **azColName)
{
  int i;
  Serial.printf("%s: ", (const char *)data);
  for (i = 1; i < argc; i++)
  {
    canvas.printf("%s: %s\n", argv[0], argv[i] ? argv[i] : "/");
    canvas.pushSprite(5, 5);
    Serial.printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
  }
  Serial.printf("\n");
  return 0;
}

int openDb(const char *filename, sqlite3 **db)
{
  int rc = sqlite3_open(filename, db);
  if (rc)
  {
    Serial.printf("Can't open database: %s\n", sqlite3_errmsg(*db));
    return rc;
  }
  else
  {
    Serial.printf("Opened database successfully\n");
  }
  return rc;
}

char *zErrMsg = 0;
int db_exec(sqlite3 *db, const char *sql, int (*callbackFn)(void *, int, char **, char **))
{
  Serial.println("Requesting:");
  Serial.println(sql);
  long start = micros();
  int rc = sqlite3_exec(db, sql, callbackFn, (void *)data, &zErrMsg);
  if (rc != SQLITE_OK)
  {
    Serial.printf("SQL error: %s\n", zErrMsg);
    sqlite3_free(zErrMsg);
  }
  else
  {
    Serial.printf("Operation done successfully\n");
  }
  Serial.print(F("Time taken:"));
  Serial.println(micros() - start);
  return rc;
}

void setup()
{
  Serial.begin(115200);
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setTextSize(TEXTSIZE);
  M5Cardputer.Display.drawRect(0, 0, M5Cardputer.Display.width(),
                               M5Cardputer.Display.height() - 28, NAVY);
  M5Cardputer.Display.setTextFont(&fonts::TomThumb);

  M5Cardputer.Display.fillRect(0, M5Cardputer.Display.height() - 4,
                               M5Cardputer.Display.width(), 4, NAVY);

  canvas.setTextFont(&fonts::TomThumb);
  canvas.setTextSize(TEXTSIZE);
  canvas.createSprite(M5Cardputer.Display.width() - 8,
                      M5Cardputer.Display.height() - 37);
  canvas.setTextScroll(true);
  SPI.begin();
  SD.begin();

  sqlite3_initialize();
  if (openDb("/sd/de-en.sqlite3", &transdb))
    return;
  if (openDb("/sd/de.sqlite3", &infodb))
    return;
}

void loop()
{
  if (!M5.Display.displayBusy())
  {
    M5Cardputer.update();
    static int prev_battery = INT_MAX;
    int battery = M5.Power.getBatteryLevel();
    int diff = abs(prev_battery - battery);
    if (diff > 10 || update_battery)
    {
      prev_battery = battery;
      M5.Display.fillRect(M5.Display.width() - 40, 0, M5.Display.width(), M5.Display.fontHeight(), BLACK);
      M5.Display.setTextColor(MAROON);
      M5.Display.startWrite();
      M5.Display.setCursor(M5.Display.width() - 40, 2);
      if (battery >= 0)
      {
        M5.Display.printf("%03d", battery);
      }
      else
      {
        M5.Display.print("none");
      }
      M5.Display.endWrite();
      M5.Display.setTextColor(WHITE);
      update_battery = false;
    }
  }
  if (M5Cardputer.Keyboard.isChange())
  {
    int now = micros();
    if (M5Cardputer.Keyboard.isPressed() && (abs(now - before) > DEBOUNCE))
    {
      Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

      for (auto i : status.word)
      {
        input_data += i;
      }

      if (status.del)
      {
        input_data.remove(input_data.length() - 1);
      }

      if (status.enter)
      {
        update_battery = true;
        input_data.remove(0, 2);
        canvas.setTextColor(YELLOW);
        canvas.fillSprite(0);
        canvas.println(input_data);
        canvas.pushSprite(5, 5);
        canvas.setTextColor(ORANGE);
        // On pressing enter, apply several replacements:
        // .vowel is replaced by vowel with umlaut
        // ss is replaced by ß. This can be problematic if there is
        // (likely there is) a German word with two blocks of ss but
        // no eszet.
        // Both for the original and the eszet versions, search for
        // capitalized and uncapitalized

        std::string searchWord(input_data.c_str());
        std::unordered_map<std::string, std::string> umlautReplacements = {
            {".a", "ä"},
            {".o", "ö"},
            {".u", "ü"}};

        for (const auto &entry : umlautReplacements)
        {
          size_t pos = 0;
          while ((pos = searchWord.find(entry.first, pos)) != std::string::npos)
          {
            searchWord.replace(pos, 2, entry.second);
          }
        }

        std::string eszet(searchWord);

        // Replace "ss" with "ß":
        size_t pos = 0;
        while ((pos = eszet.find("ss", pos)) != std::string::npos)
        {
          eszet.replace(pos, 2, "ß");
          pos += 1; // Move to the next character after the replacement
        }

        std::string capitalized_original(searchWord);
        capitalized_original[0] = std::toupper(capitalized_original[0]);

        std::string capitalized_eszet(eszet);
        capitalized_eszet[0] = std::toupper(capitalized_eszet[0]);
        snprintf(queries[0], MAX_QUERY_LENGTH, "Select written_rep, trans_list from translation where written_rep in ('%s', '%s', '%s', '%s') limit 6", searchWord.c_str(), capitalized_original.c_str(), eszet.c_str(), capitalized_eszet.c_str());
        snprintf(queries[1], MAX_QUERY_LENGTH, "Select written_rep, part_of_speech, gender from entry where written_rep in ('%s', '%s', '%s', '%s')  and substr(lexentry, 1, 3) = 'deu' limit 6", searchWord.c_str(), capitalized_original.c_str(), eszet.c_str(), capitalized_eszet.c_str());
        int rc = db_exec(transdb, queries[0], callbackTrans);
        if (rc != SQLITE_OK)
        {
          sqlite3_close(transdb);
          return;
        }
        input_data = "> ";
      }

      if (input_data.length() > 0 && input_data.charAt(input_data.length() - 1) == '/')
      {
        // Pressing the / key switches to the alternate dictionary, with gender
        update_battery = true;
        input_data = "> ";
        canvas.fillSprite(0); // This supposedly clears the canvas
        int rc = db_exec(infodb, queries[1], callbackTrans);
        if (rc != SQLITE_OK)
        {
          sqlite3_close(infodb);
          return;
        }
      }

      if (input_data.length() > 0 && input_data.charAt(input_data.length() - 1) == ',')
      {
        // Pressing the , key switches to the original (translation) dictionary
        update_battery = true;
        input_data = "> ";
        canvas.fillSprite(0);
        int rc = db_exec(transdb, queries[0], callbackTrans);
        if (rc != SQLITE_OK)
        {
          sqlite3_close(transdb);
          return;
        }
      }

      M5Cardputer.Display.fillRect(0, M5Cardputer.Display.height() - 28,
                                   M5Cardputer.Display.width(), 25,
                                   BLACK);

      M5Cardputer.Display.drawString(input_data, 5,
                                     M5Cardputer.Display.height() - 23);
    }
  }
}