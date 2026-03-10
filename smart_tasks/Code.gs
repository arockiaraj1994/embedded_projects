/**
 * Google Apps Script — Smart Tasks API for ESP32 E-Paper
 *
 * Setup:
 *   1. Go to https://script.google.com and create a new project
 *   2. Paste this file as Code.gs
 *   3. In the editor, click Services (+) on the left sidebar
 *   4. Add "Google Tasks API" (Tasks API v1)
 *   5. Deploy → New deployment → Web app
 *      - Execute as: Me
 *      - Who has access: Anyone
 *   6. Copy the deployment URL and paste it into smart_tasks.ino as APPS_SCRIPT_URL
 *   7. Copy the API_KEY below and paste it into smart_tasks.ino as API_KEY
 */

const API_KEY = "f47ac10b-58cc-4372-a567-0e02b2c3d479";
const MAX_TASKS = 10;

function doGet(e) {
  var key = e && e.parameter && e.parameter.key;
  if (key !== API_KEY) {
    return ContentService
      .createTextOutput(JSON.stringify({ error: "Unauthorized" }))
      .setMimeType(ContentService.MimeType.JSON);
  }

  try {
    var taskLists = Tasks.Tasklists.list();
    if (!taskLists.items || taskLists.items.length === 0) {
      return jsonResponse({ count: 0, tasks: [] });
    }

    var listId = taskLists.items[0].id;

    var result = Tasks.Tasks.list(listId, {
      showCompleted: false,
      showHidden: false,
      maxResults: MAX_TASKS
    });

    var items = result.items || [];

    var tasks = items.map(function(t) {
      var due = t.due ? t.due.substring(0, 10) : null;
      var notes = t.notes || null;
      if (notes && notes.length > 60) {
        notes = notes.substring(0, 57) + "...";
      }
      return {
        title: t.title || "(untitled)",
        due: due,
        notes: notes
      };
    });

    tasks.sort(function(a, b) {
      if (a.due && b.due) return a.due.localeCompare(b.due);
      if (a.due) return -1;
      if (b.due) return 1;
      return 0;
    });

    return jsonResponse({ count: tasks.length, tasks: tasks });

  } catch (err) {
    return jsonResponse({ error: err.message, count: 0, tasks: [] });
  }
}

function jsonResponse(obj) {
  return ContentService
    .createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}
