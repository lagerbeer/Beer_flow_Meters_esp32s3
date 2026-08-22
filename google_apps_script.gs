// ============================================================
// Beer Flow Monitor — Google Sheets logger
//
// Setup:
//   1. Create a new Google Sheet (sheets.new).
//   2. Extensions > Apps Script. Delete any starter code and paste this file in.
//   3. Click Deploy > New deployment.
//        - Type: Web app
//        - Execute as: Me
//        - Who has access: Anyone
//   4. Click Deploy, authorize when prompted, then copy the Web app URL
//      (ends in /exec).
//   5. Paste that URL into the dashboard's Integrations tab -> Google Sheets,
//      check "Log Every Pour", and hit Save. Use the Test button to confirm
//      a row appears.
//
// Every pour appends one row to a "Pours" sheet (created automatically on
// first use) with a header row on first write.
// ============================================================

const SHEET_NAME = 'Pours';

const COLUMNS = [
  'Received At', 'Pour DateTime', 'Tap', 'Tap Name', 'Beer', 'ABV %', 'IBU',
  'Ounces', 'Duration (s)', 'Peak Flow (oz/s)',
  'Keg Level (oz)', 'Keg Level (gal)', 'Keg Capacity (oz)', 'Keg %',
];

function doPost(e) {
  const sheet = getOrCreateSheet_();
  const data = JSON.parse(e.postData.contents);

  sheet.appendRow([
    new Date(),
    data.dateTime    || '',
    data.tap          ?? '',
    data.tapName     || '',
    data.beer        || '',
    data.abv          ?? '',
    data.ibu          ?? '',
    data.ounces       ?? '',
    data.duration     ?? '',
    data.peakFlow     ?? '',
    data.kegLevel     ?? '',
    data.kegLevelGal  ?? '',
    data.kegCapacity  ?? '',
    data.kegPct       ?? '',
  ]);

  return ContentService
    .createTextOutput(JSON.stringify({ success: true }))
    .setMimeType(ContentService.MimeType.JSON);
}

function getOrCreateSheet_() {
  const ss = SpreadsheetApp.getActiveSpreadsheet();
  let sheet = ss.getSheetByName(SHEET_NAME);
  if (!sheet) {
    sheet = ss.insertSheet(SHEET_NAME);
    sheet.appendRow(COLUMNS);
    sheet.setFrozenRows(1);
  }
  return sheet;
}
