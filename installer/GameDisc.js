// GameDisc.js - Windows Installer JScript custom actions for the "Game Disc" setup page.
// Add this file to the Binary table (e.g. as GameDiscJS) and create two custom actions
// (type: JScript stored in Binary table, immediate):
//   FindGameDisc   -> function FindGameDisc   (Next button of the page before the Game Disc
//                     page: pre-selects the drive labelled RUGRATS that holds the game)
//   CheckGameDisc  -> function CheckGameDisc  (Next button of the Game Disc page:
//                     sets GAMEDISC_OK to "1" if GAMEDISC holds the game, else "0")
//   FindInstalledGame -> function FindInstalledGame (upgrades: InstallUISequence and
//                     InstallExecuteSequence, after FindRelatedProducts, before CostInitialize)
// Plain JScript (jscript.dll), so it doesn't depend on VBScript, which Windows is phasing out.

// Diagnostics: each line goes to the Windows Installer log (search for "GameDisc:") and is also
// appended to the property GAMEDISC_TRACE, which the log lists at the end of every run.
function Log(msg) {
    try {
        var t = Session.Property("GAMEDISC_TRACE");
        Session.Property("GAMEDISC_TRACE") = (t != "" ? t + " | " : "") + msg;
    } catch (e) { }
    try {
        var rec = Session.Installer.CreateRecord(0);
        rec.StringData(0) = "GameDisc: " + msg;
        Session.Message(0x04000000, rec);   // INSTALLMESSAGE_INFO
    } catch (e) { }
}

// The files the port needs from the disc: the first episode archive and the game's outline.
var GAME_FILES = ["MHK\\EPISODE0.MHK", "PROGRAM\\OUTLINE"];

function IsGameFolder(fso, d) {
    for (var i = 0; i < GAME_FILES.length; ++i)
        if (!fso.FileExists(d + GAME_FILES[i])) { Log("  missing " + d + GAME_FILES[i]); return false; }
    return true;
}

function WithSlash(d) {
    if (d.length && d.charAt(d.length - 1) != "\\") d += "\\";
    return d;
}

// The original disc's volume label(s), as shown in File Explorer (not case-sensitive).
var DISC_LABELS = ["RUGRATS"];   // the 1998 CD-ROM's label

// Pre-selects the game disc on the Game Disc page. Run it from the Next button of the page
// before (DoAction FindGameDisc, then NewDialog to the Game Disc page), so a disc inserted
// while the player reads the earlier pages is still found.
// Only CD/DVD drives are considered (a mounted ISO of the disc counts as one): the first ready
// one labelled like the original disc that also holds the game's files is selected. Hard
// disks, network and removable drives are never picked automatically. If no disc is found,
// the field is left as it is and the player uses Browse. A path already in the field (typed,
// browsed or given on the command line) is kept unless it no longer holds the game.
function FindGameDisc() {
    try {
        var fso = new ActiveXObject("Scripting.FileSystemObject");
        var cur = WithSlash(Session.Property("GAMEDISC"));
        Log("FindGameDisc start, GAMEDISC='" + cur + "'");
        if (cur != "" && IsGameFolder(fso, cur)) { Log("kept current path"); return 1; }
        for (var e = new Enumerator(fso.Drives); !e.atEnd(); e.moveNext()) {
            var drv = e.item();
            var info = drv.DriveLetter + ": type " + drv.DriveType + (drv.IsReady ? ", label '" + drv.VolumeName + "'" : ", not ready");
            if (drv.DriveType != 4 || !drv.IsReady || !HasDiscLabel(drv)) { Log(info + " - skipped"); continue; }   // 4 = CD/DVD
            var root = drv.DriveLetter + ":\\";
            if (IsGameFolder(fso, root)) { Session.Property("GAMEDISC") = root; Log(info + " - selected"); return 1; }
            Log(info + " - label matches but game files not found");
        }
        Log("no game disc found");
    } catch (err) { Log("FindGameDisc error: " + err.message); }
    return 1;   // never fail the install from here
}

function HasDiscLabel(drv) {
    var label = String(drv.VolumeName).toUpperCase();
    for (var i = 0; i < DISC_LABELS.length; ++i)
        if (label == DISC_LABELS[i].toUpperCase()) return true;
    return false;
}

// Checks the folder the player chose. GAMEDISC_OK = "1" when MHK\EPISODE0.MHK,
// PROGRAM\OUTLINE are there; GAMEDISC gets a trailing backslash.
function CheckGameDisc() {
    var ok = false;
    try {
        var fso = new ActiveXObject("Scripting.FileSystemObject");
        var d = WithSlash(Session.Property("GAMEDISC"));
        ok = d != "" && IsGameFolder(fso, d);
        if (ok) Session.Property("GAMEDISC") = d;
        Log("CheckGameDisc '" + d + "' -> " + (ok ? "OK" : "not the game"));
    } catch (err) { ok = false; Log("CheckGameDisc error: " + err.message); }
    Session.Property("GAMEDISC_OK") = ok ? "1" : "0";
    return 1;
}

// Upgrades (1.1 and later over an earlier version): the game's files copied from the disc
// stay in the earlier version's folder (its uninstall leaves them while upgrading). If that
// folder still holds them, GAMEDATA_FOUND = "1" and INSTALLDIR is set to that folder, so the
// new version goes back in the same place and the disc page and the copy from the disc are
// skipped. UPGRADE_PROPERTY is the Upgrade table's action property (set by FindRelatedProducts
// to the earlier version's product code).
var UPGRADE_PROPERTY = "UPGRADE_1";
var GAME_FOLDERS = ["PROGRAM\\MHK"];

function FindInstalledGame() {
    try {
        var fso = new ActiveXObject("Scripting.FileSystemObject");
        var codes = String(Session.Property(UPGRADE_PROPERTY)).split(";");
        Log("FindInstalledGame start, " + UPGRADE_PROPERTY + "='" + codes.join(";") + "'");
        for (var i = 0; i < codes.length; ++i) {
            var code = codes[i];
            if (code == "") continue;
            var loc = "";
            try { loc = String(Session.Installer.ProductInfo(code, "InstallLocation")); } catch (e) { loc = ""; }
            loc = WithSlash(loc);
            if (loc == "") { Log("  " + code + ": no install location recorded"); continue; }
            var ok = IsGameFolder(fso, loc);
            for (var k = 0; ok && k < GAME_FOLDERS.length; ++k)
                if (!fso.FolderExists(loc + GAME_FOLDERS[k])) { Log("  missing " + loc + GAME_FOLDERS[k]); ok = false; }
            if (!ok) { Log("  " + code + ": '" + loc + "' doesn't hold the game's files"); continue; }
            Session.Property("INSTALLDIR") = loc;
            Session.Property("GAMEDATA_FOUND") = "1";
            Log("  " + code + ": game files found in '" + loc + "' - reusing them");
            return 1;
        }
        Log("no installed game files found");
    } catch (err) { Log("FindInstalledGame error: " + err.message); }
    return 1;   // never fail the install from here
}
