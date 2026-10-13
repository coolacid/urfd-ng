<?php
// Last heard: the LCD with who is on the air, the module keypad and the log

$ShowFilter = !empty($PageOptions['UserPage']['ShowFilter']);

// modules for the keypad: the configured ones, else the ones nodes are linked to
$Keys = $Modules;
if (count($Keys) == 0) {
    foreach ($Reflector->GetModules() as $m) $Keys[$m] = array('name' => '', 'nodes' => count($Reflector->GetNodesInModulesById($m)), 'onair' => '');
    ksort($Keys);
}

// what the LCD shows: the station on the air, else the last one heard
$LcdStation = null;
$LcdCall = '';
$LcdMod = '';
if ($OnAir !== null) {
    list($LcdMod, $LcdCall, $LcdStation) = $OnAir;
} else if ($Reflector->GetStation(0) !== false) {
    $LcdStation = $Reflector->GetStation(0);
    $LcdCall = $LcdStation->GetCallsignOnly();
    $LcdMod = $LcdStation->GetModule();
}

$LcdClass = 'lcd'.(($OnAir === null) ? ' idle' : '').(($LcdCall == '') ? ' empty' : '');
?>
<div class="page">
    <section class="deck" aria-label="On the air">
        <div class="bezel">
            <div class="<?php echo $LcdClass; ?>" id="lcd" data-call="<?php echo h($LcdMod.$LcdCall); ?>" aria-live="polite">
                <div class="lcd-top">
<?php if ($OnAir !== null) { ?>
                    <span class="ann">TX</span><span>Transmitting</span>
                    <?php if ($LcdStation) { ?><span class="lcd-timer" data-since="<?php echo (int)$LcdStation->GetLastHeardTime(); ?>"></span><?php } ?>
<?php } else if ($LcdStation) { ?>
                    <span>Last heard <?php echo FormatAgo($LcdStation->GetLastHeardTime()); ?></span>
<?php } else { ?>
                    <span>Nobody heard yet</span>
<?php } ?>
                </div>
<?php if ($LcdCall != '') {
    $sfx = $LcdStation ? $LcdStation->GetSuffix() : '';
?>
                <p class="lcd-call"><?php echo h($LcdCall); ?><?php if ($sfx != '') echo '<small>/'.h($sfx).'</small>'; ?></p>
                <div class="lcd-ch"><span>Module</span><b><?php echo h($LcdMod); ?></b></div>
                <dl class="lcd-fields">
                    <div><dt>Mode</dt><dd><?php echo ($LcdStation && $LcdStation->GetProtocol() != '') ? h($LcdStation->GetProtocol()) : 'unknown'; ?></dd></div>
                    <div><dt>Via</dt><dd><?php echo $LcdStation ? h(StationVia($Reflector, $LcdStation)) : 'unknown'; ?></dd></div>
                    <?php if ($LcdStation && $LcdStation->GetLocator() != '') { ?><div><dt>Locator</dt><dd><?php echo h($LcdStation->GetLocator()); ?></dd></div><?php } ?>
                </dl>
<?php } else { ?>
                <p class="lcd-call">Waiting for the first call</p>
<?php } ?>
            </div>
        </div>

        <div>
            <div class="keys-head">
                <h2>Modules</h2>
                <?php if ($ShowFilter) { ?><p class="hint">Tap one to filter the log</p><?php } ?>
            </div>
            <div class="keys" id="keys">
<?php
$n = 0;
foreach ($Keys as $mod => $info) {
    $nodes = (int)$info['nodes'];
    $tx = ($info['onair'] != '');
    $cls = 'key'.($nodes > 0 ? ' busy' : '').($tx ? ' tx' : '');
    $label = 'Module '.$mod.($info['name'] != '' ? ', '.$info['name'] : '').', '.$nodes.($nodes == 1 ? ' node' : ' nodes').($tx ? ', transmitting' : '');
    $inner = '<i class="led"></i><span class="l">'.h($mod).'</span><span class="n">'.$nodes.'</span><span class="d">'.h($info['name']).'</span>';
    if ($ShowFilter) {
        echo '<button type="button" class="'.$cls.'" data-m="'.h($mod).'" aria-pressed="false" aria-label="'.h($label).'">'.$inner.'</button>';
    } else {
        echo '<div class="'.$cls.'" aria-label="'.h($label).'" role="img">'.$inner.'</div>';
    }
    $n++;
}
while ($n == 0 || $n % 4) { echo '<span class="key blank" aria-hidden="true"></span>'; $n++; }
?>
            </div>
        </div>
    </section>

    <section aria-labelledby="log-title">
        <div class="tbl-head">
            <h2 id="log-title">Last heard</h2>
<?php if ($ShowFilter) { ?>
            <div class="filters">
                <label for="f-call">Callsign</label>
                <input class="field" id="f-call" type="search" placeholder="DL*, *JC" autocomplete="off" spellcheck="false">
                <button class="chip" id="f-clear" type="button" hidden>Clear filters</button>
            </div>
<?php } ?>
        </div>
        <div class="scroll">
            <table class="t" id="log">
                <thead><tr><th class="state"><span class="sr">State</span></th><th>Callsign</th><th>Module</th><th>Mode</th><th>Via</th><th class="opt">Position</th><th class="r">Heard</th></tr></thead>
                <tbody>
<?php
$Limit = (int)$PageOptions['LastHeardPage']['LimitTo'] + 1;
$liveDone = array();
$count = min($Reflector->StationCount(), $Limit);
for ($i = 0; $i < $count; $i++) {
    $st = $Reflector->Stations[$i];
    $cs = $st->GetCallsignOnly();
    $mod = $st->GetModule();
    $live = isset($Modules[$mod]) && $Modules[$mod]['onair'] == trim($cs) && !isset($liveDone[$mod]);
    if ($live) $liveDone[$mod] = true;
    list($flag, $country) = $Reflector->GetFlag($st->GetCallSign());
    $via = StationVia($Reflector, $st);
    $proto = $st->GetProtocol();

    echo '<tr'.($live ? ' class="live"' : '').' data-m="'.h($mod).'" data-cs="'.h($cs).'">';
    echo '<td class="state">'.($live ? '<span class="txtag">TX</span>' : '').'</td>';
    echo '<td class="cs"><a href="https://www.qrz.com/db/'.rawurlencode($cs).'" target="_blank" rel="noopener">'.h($cs).'</a>';
    if ($st->GetSuffix() != '') echo '<span class="sfx">/'.h($st->GetSuffix()).'</span>';
    if ($flag != '') echo '<span class="cc" title="'.h($country).'">'.h(strtoupper($flag)).'</span>';
    echo '</td>';
    echo '<td class="m"><span class="mod'.($live ? ' on' : '').'">'.h($mod).'</span></td>';
    echo '<td class="p">'.h($proto).'</td>';
    echo '<td class="v">'.h($via).'</td>';
    echo '<td class="meta">'.h(trim($proto.($via != '' ? ' via '.$via : ''))).'</td>';
    echo '<td class="opt">';
    if ($st->GetLocator() != '') {
        // position sent along with the transmission (M17 GNSS, D-Star DPRS), rounded to its locator
        $lat = (float)$st->GetLatitude();
        $lon = (float)$st->GetLongitude();
        echo '<a href="https://www.openstreetmap.org/?mlat='.$lat.'&amp;mlon='.$lon.'#map=12/'.$lat.'/'.$lon.'" target="_blank" rel="noopener" title="Position sent with the transmission">'.h($st->GetLocator()).'</a>';
    } else {
        echo '<a class="muted" href="https://aprs.fi/'.rawurlencode($cs).'" target="_blank" rel="noopener">aprs.fi</a>';
    }
    echo '</td>';
    echo '<td class="r when" title="'.h(date("d.m.Y H:i", $st->GetLastHeardTime())).'">'.($live ? 'on the air' : FormatAgo($st->GetLastHeardTime())).'</td>';
    echo '</tr>';
}
if ($count == 0) echo '<tr class="empty"><td colspan="7">Nobody heard since the reflector started. Calls show up here as soon as someone keys up.</td></tr>';
?>
                    <tr class="empty" id="log-none" hidden><td colspan="7">Nobody in the log matches these filters. Clear the filters to see all stations.</td></tr>
                </tbody>
            </table>
        </div>
    </section>
</div>

<script>
(function () {
    // filters live in the browser so the page refresh keeps them
    var state = { m: null, cs: '' }, lastCall = null;
    try { var s = JSON.parse(sessionStorage.getItem('urf-filter') || 'null'); if (s) state = s; } catch (e) {}

    function glob(pat, s) {
        if (!pat) return true;
        if (pat.indexOf('*') < 0) pat = '*' + pat + '*';
        return new RegExp('^' + pat.split('*').map(function (p) { return p.replace(/[.+?^${}()|[\]\\]/g, '\\$&'); }).join('.*') + '$', 'i').test(s);
    }
    function apply() {
        var input = document.getElementById('f-call');
        if (input && input !== document.activeElement) input.value = state.cs;
        document.querySelectorAll('#keys button.key').forEach(function (k) { k.setAttribute('aria-pressed', String(k.dataset.m === state.m)); });
        var shown = 0, rows = document.querySelectorAll('#log tbody tr[data-m]');
        rows.forEach(function (tr) {
            var ok = (!state.m || tr.dataset.m === state.m) && glob(state.cs, tr.dataset.cs);
            tr.hidden = !ok; if (ok) shown++;
        });
        var none = document.getElementById('log-none'); if (none) none.hidden = !(rows.length && !shown);
        var clear = document.getElementById('f-clear'); if (clear) clear.hidden = !(state.m || state.cs);
        try { sessionStorage.setItem('urf-filter', JSON.stringify(state)); } catch (e) {}

        // a new call on the LCD flickers once, like the display switching over
        var lcd = document.getElementById('lcd');
        if (lcd) {
            if (lastCall !== null && lcd.dataset.call !== lastCall) { lcd.classList.remove('flick'); void lcd.offsetWidth; lcd.classList.add('flick'); }
            lastCall = lcd.dataset.call;
        }
    }
    document.addEventListener('click', function (e) {
        var k = e.target.closest('#keys button.key');
        if (k) { state.m = (state.m === k.dataset.m) ? null : k.dataset.m; apply(); return; }
        if (e.target.closest('#f-clear')) { state = { m: null, cs: '' }; var i = document.getElementById('f-call'); if (i) i.value = ''; apply(); }
    });
    document.addEventListener('input', function (e) { if (e.target.id === 'f-call') { state.cs = e.target.value.trim(); apply(); } });
    window.URF.onRefresh.push(apply);
    apply();
})();
</script>
