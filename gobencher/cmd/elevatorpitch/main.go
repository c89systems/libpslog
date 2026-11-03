package main

import (
	"bytes"
	"context"
	"flag"
	"fmt"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unicode/utf8"

	"github.com/c89systems/libpslog/gobencher/benchmark"
	"pkt.systems/pslog/ansi"
)

const (
	defaultDuration       = 5 * time.Second
	defaultUpdateInterval = 100 * time.Millisecond
	initialBlockSize      = uint64(5000)
	cRunChunk             = 4096
	maxChartRows          = 20
	barCell               = "████"
	columnWidth           = 10
	barWidth              = 5
	axisWidth             = 8
	maxLines              = 20_000_000
)

var (
	loggerOrder []string

	barLeftPad  = (columnWidth - barWidth) / 2
	barRightPad = columnWidth - barWidth - barLeftPad

	diagramPalette []diagramColor
)

type diagramColor struct {
	Bar      string
	Label    string
	RankBold string
}

type runnerMetrics struct {
	name  string
	lines atomic.Uint64
	bytes atomic.Uint64
}

type runner struct {
	name string
	run  func(ctx context.Context, metrics *runnerMetrics)
}

func main() {
	duration := flag.Duration("duration", defaultDuration, "per-logger duration to measure")
	updateInterval := flag.Duration("interval", defaultUpdateInterval, "UI refresh interval")
	limit := flag.Int("limit", benchmark.DefaultProductionLimit, "limit number of log entries to replay")
	maxLinesCap := flag.Uint64("max-lines", maxLines, "cap for chart Y axis (lines per run)")
	includeQuill := flag.Bool("include-quill", false, "include optional Quill compare in the chart")
	diagramPaletteName := flag.String("diagram-palette", "solarized", "diagram palette (monokai|synthwave84|solarized|catppuccin|dracula|gruvboxlight|gruvbox|tokyo|outrun|nord|material|everforest|one-dark)")
	flag.Parse()

	entries, err := benchmark.LoadProductionEntries(*limit)
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to load production dataset: %v\n", err)
		os.Exit(1)
	}
	diagramPalette = diagramPaletteFor(*diagramPaletteName)
	loggerOrder = benchmark.ComparisonNames()
	if *includeQuill {
		loggerOrder = benchmark.ComparisonNamesWithQuill()
	}

	staticFields, staticKeys := benchmark.ProductionStaticFields(entries)
	dynamicEntries := benchmark.ProductionEntriesWithout(entries, staticKeys)
	preparedAll := benchmark.NewCPreparedData(entries)
	defer preparedAll.Close()
	preparedDynamic := benchmark.NewCPreparedData(dynamicEntries)
	defer preparedDynamic.Close()
	preparedStatic := preparedDynamic.PrepareFields(staticFields)
	var luaAll *benchmark.LuaData
	var luaDynamic *benchmark.LuaData
	if benchmark.HasLuaPSLog() {
		luaAll = benchmark.NewLuaData(entries)
		luaDynamic = benchmark.NewLuaData(dynamicEntries)
		defer luaAll.Close()
		defer luaDynamic.Close()
	}

	variants := benchmark.Variants()
	useWith := len(staticFields) > 0
	runners := []runner{
		{name: "jsonGo", run: goRunner(variants[0], dynamicEntries, staticFields, entries, useWith)},
		{name: "jsonC", run: cRunner(variants[0], preparedDynamic, preparedStatic, preparedAll, useWith)},
		{name: "coljsonGo", run: goRunner(variants[1], dynamicEntries, staticFields, entries, useWith)},
		{name: "coljsonC", run: cRunner(variants[1], preparedDynamic, preparedStatic, preparedAll, useWith)},
		{name: "consoleGo", run: goRunner(variants[2], dynamicEntries, staticFields, entries, useWith)},
		{name: "consoleC", run: cRunner(variants[2], preparedDynamic, preparedStatic, preparedAll, useWith)},
		{name: "colconGo", run: goRunner(variants[3], dynamicEntries, staticFields, entries, useWith)},
		{name: "colconC", run: cRunner(variants[3], preparedDynamic, preparedStatic, preparedAll, useWith)},
	}
	if luaAll != nil && luaDynamic != nil {
		runners = append(runners, runner{name: "jsonLua", run: luaRunner(luaDynamic, luaAll, staticFields, useWith)})
	}
	if *includeQuill && benchmark.HasQuill() {
		quillData := benchmark.NewQuillData(entries)
		defer quillData.Close()
		runners = append(runners, runner{name: "jsonQuill", run: quillRunner(quillData)})
	}

	runLiveBenchmark(runners, loggerOrder, *duration, *updateInterval, *maxLinesCap)
}

func goRunner(variant benchmark.Variant, dynamicEntries []benchmark.Entry, staticFields []benchmark.Field, allEntries []benchmark.Entry, useWith bool) func(context.Context, *runnerMetrics) {
	return func(ctx context.Context, metrics *runnerMetrics) {
		sink := benchmark.NewSink()
		logger := benchmark.NewGoLogger(sink, variant)
		activeEntries := allEntries
		if useWith {
			logger = logger.With(benchmark.FieldsToKeyvals(staticFields)...)
			activeEntries = dynamicEntries
		}
		replayEntries(ctx, activeEntries, metrics, func(entry benchmark.Entry) {
			entry.LogGo(logger)
			metrics.bytes.Store(uint64(sink.BytesWritten()))
		})
	}
}

func cRunner(variant benchmark.Variant, dynamic *benchmark.CPreparedData, static benchmark.CPreparedFields, all *benchmark.CPreparedData, useWith bool) func(context.Context, *runnerMetrics) {
	return func(ctx context.Context, metrics *runnerMetrics) {
		logger, err := benchmark.NewCLogger(variant)
		if err != nil {
			fmt.Fprintf(os.Stderr, "new C logger %s: %v\n", variant.Name, err)
			return
		}
		defer logger.Close()

		activeData := all
		activeLogger := logger
		if useWith {
			withLogger, err := logger.WithPrepared(static)
			if err != nil {
				fmt.Fprintf(os.Stderr, "with prepared C logger %s: %v\n", variant.Name, err)
				return
			}
			defer withLogger.Close()
			activeLogger = withLogger
			activeData = dynamic
		}

		for {
			select {
			case <-ctx.Done():
				return
			default:
			}
			result, err := activeLogger.RunPrepared(activeData, cRunChunk)
			if err != nil {
				fmt.Fprintf(os.Stderr, "run prepared C logger %s: %v\n", variant.Name, err)
				return
			}
			metrics.lines.Add(uint64(result.Ops))
			metrics.bytes.Add(result.BytesWritten)
		}
	}
}

func luaRunner(dynamic *benchmark.LuaData, all *benchmark.LuaData, staticFields []benchmark.Field, useWith bool) func(context.Context, *runnerMetrics) {
	return func(ctx context.Context, metrics *runnerMetrics) {
		activeData := all
		activeStatic := []benchmark.Field(nil)
		if useWith {
			activeData = dynamic
			activeStatic = staticFields
		}

		logger, err := benchmark.NewLuaLogger(activeStatic, true)
		if err != nil {
			fmt.Fprintf(os.Stderr, "new Lua logger: %v\n", err)
			return
		}
		defer logger.Close()

		prepared, err := benchmark.PrepareLuaData(logger, activeData)
		if err != nil {
			fmt.Fprintf(os.Stderr, "prepare Lua benchmark data: %v\n", err)
			return
		}
		defer prepared.Close()

		for {
			select {
			case <-ctx.Done():
				return
			default:
			}
			result, err := benchmark.RunLuaPrepared(logger, prepared, cRunChunk)
			if err != nil {
				fmt.Fprintf(os.Stderr, "run Lua benchmark: %v\n", err)
				return
			}
			metrics.lines.Add(result.Ops)
			metrics.bytes.Add(result.BytesWritten)
		}
	}
}

func quillRunner(data *benchmark.QuillData) func(context.Context, *runnerMetrics) {
	return func(ctx context.Context, metrics *runnerMetrics) {
		for {
			select {
			case <-ctx.Done():
				return
			default:
			}
			result, err := benchmark.RunQuill(data, cRunChunk)
			if err != nil {
				fmt.Fprintf(os.Stderr, "run quill benchmark: %v\n", err)
				return
			}
			metrics.lines.Add(result.Ops)
			metrics.bytes.Add(result.BytesWritten)
		}
	}
}

func runLiveBenchmark(runners []runner, order []string, runDuration, updateInterval time.Duration, maxLinesCap uint64) {
	metrics, _ := newMetrics(order)

	ctx, cancel := context.WithTimeout(context.Background(), runDuration)
	defer cancel()

	fmt.Print("\033[?25l")
	defer fmt.Print("\033[?25h")

	start := time.Now()

	var wg sync.WaitGroup
	for i, current := range runners {
		wg.Add(1)
		go func(run runner, metric *runnerMetrics) {
			defer wg.Done()
			run.run(ctx, metric)
		}(current, metrics[i])
	}

	linesPrinted := renderLoop(ctx, metrics, updateInterval, runDuration, runDuration, start, maxLinesCap)
	cancel()
	wg.Wait()

	finalizeDisplay(metrics, runDuration, runDuration, maxLinesCap, linesPrinted)
}

func newMetrics(order []string) ([]*runnerMetrics, map[string]*runnerMetrics) {
	metrics := make([]*runnerMetrics, len(order))
	metricsMap := make(map[string]*runnerMetrics, len(order))
	for i, name := range order {
		metric := &runnerMetrics{name: name}
		metrics[i] = metric
		metricsMap[name] = metric
	}
	return metrics, metricsMap
}

func replayEntries[T any](ctx context.Context, entries []T, metrics *runnerMetrics, emit func(T)) {
	if len(entries) == 0 {
		return
	}
	index := 0
	for {
		select {
		case <-ctx.Done():
			return
		default:
		}
		entry := entries[index]
		index++
		if index == len(entries) {
			index = 0
		}
		metrics.lines.Add(1)
		emit(entry)
	}
}

func renderLoop(ctx context.Context, metrics []*runnerMetrics, interval, totalDuration, runDuration time.Duration, start time.Time, maxLinesCap uint64) int {
	ticker := time.NewTicker(interval)
	defer ticker.Stop()

	var linesPrinted int
	first := true
	for {
		now := time.Now()
		elapsed := minDuration(now.Sub(start), totalDuration)
		buf, lineCount := buildDisplay(metrics, elapsed, totalDuration, runDuration, maxLinesCap, false)
		if first {
			first = false
		} else if linesPrinted > 0 {
			fmt.Printf("\033[%dA", linesPrinted)
		}
		_, _ = os.Stdout.Write(buf.Bytes())
		linesPrinted = lineCount

		if ctx.Err() != nil {
			return linesPrinted
		}

		select {
		case <-ctx.Done():
			return linesPrinted
		case <-ticker.C:
		}
	}
}

func finalizeDisplay(metrics []*runnerMetrics, totalDuration, runDuration time.Duration, maxLinesCap uint64, linesPrinted int) {
	finalBuf, finalLines := buildDisplay(metrics, totalDuration, totalDuration, runDuration, maxLinesCap, true)
	if linesPrinted > 0 {
		fmt.Printf("\033[%dA", linesPrinted)
	}
	_, _ = os.Stdout.Write(finalBuf.Bytes())
	if finalLines > 0 {
		linesPrinted = finalLines
		_ = linesPrinted
	}
	fmt.Println()
	printSummary(metrics, runDuration)
}

func buildDisplay(metrics []*runnerMetrics, elapsed, totalDuration, runDuration time.Duration, maxLinesCap uint64, showRanks bool) (*bytes.Buffer, int) {
	snapshots := snapshotMetrics(metrics)
	effective := make([]uint64, len(snapshots))

	var maxEffective uint64
	for i, snap := range snapshots {
		value := snap.lines
		if maxLinesCap > 0 && value > maxLinesCap {
			value = maxLinesCap
		}
		effective[i] = value
		if value > maxEffective {
			maxEffective = value
		}
	}

	ranks := computeRanks(snapshots)
	blockValue, height := computeScale(maxEffective, maxLinesCap)

	var buf bytes.Buffer
	lines := 0

	buf.WriteString(colorizeAxis(fmt.Sprintf("Y = loglines (per %s run)       each %s = %s loglines\n", formatDuration(runDuration), barCell, formatNumber(blockValue))))
	lines++
	totalWidth := axisWidth + columnWidth*len(snapshots)
	buf.WriteString(colorizeFrame("┌"))
	buf.WriteString(colorizeFrame(strings.Repeat("─", totalWidth)))
	buf.WriteString(colorizeFrame("┐\n"))
	lines++

	rankRows := make([]int, len(snapshots))
	if showRanks {
		for i, value := range effective {
			if value == 0 {
				continue
			}
			rankRows[i] = int(math.Ceil(float64(value) / float64(blockValue)))
		}
	}

	for row := height; row >= 1; row-- {
		threshold := uint64(row) * blockValue
		var lower uint64
		if row > 1 {
			lower = uint64(row-1) * blockValue
		}
		label := fmt.Sprintf("%5s ┤", formatNumber(threshold))
		buf.WriteString(colorizeFrame("│"))
		buf.WriteString(colorizeAxis(padRight(label, axisWidth)))
		for i := range snapshots {
			if showRanks && rankRows[i] > 0 && row == rankRows[i]+1 {
				buf.WriteString(colorizeRank(i, centerText(strconv.Itoa(ranks[i]), columnWidth)))
			} else {
				buf.WriteString(renderBarCell(i, effective[i], lower, threshold))
			}
		}
		buf.WriteString(colorizeFrame("│\n"))
		lines++
	}

	zeroLabel := fmt.Sprintf("%5s ┼", "0")
	buf.WriteString(colorizeFrame("│"))
	buf.WriteString(colorizeAxis(padRight(zeroLabel, axisWidth)))
	for range snapshots {
		buf.WriteString(colorizeAxis(strings.Repeat("─", columnWidth)))
	}
	buf.WriteString(colorizeFrame("┤\n"))
	lines++

	buf.WriteString(colorizeFrame("│"))
	buf.WriteString(strings.Repeat(" ", axisWidth))
	for i, snap := range snapshots {
		buf.WriteString(colorizeLabel(i, centerText(snap.name, columnWidth)))
	}
	buf.WriteString(colorizeFrame("│\n"))
	lines++

	buf.WriteString(colorizeFrame("│"))
	buf.WriteString(strings.Repeat(" ", axisWidth))
	for i, snap := range snapshots {
		lineText := fmt.Sprintf("%s lines", formatNumber(snap.lines))
		buf.WriteString(colorizeLabel(i, centerText(lineText, columnWidth)))
	}
	buf.WriteString(colorizeFrame("│\n"))
	lines++

	buf.WriteString(colorizeFrame("│"))
	elapsedText := fmt.Sprintf("%s/%s", formatDuration(elapsed), formatDuration(totalDuration))
	buf.WriteString(colorizeAxis(centerText(elapsedText, axisWidth)))
	window := elapsed
	if window <= 0 {
		window = time.Millisecond
	}
	for i, snap := range snapshots {
		perSecond := float64(snap.lines) / window.Seconds()
		rateText := fmt.Sprintf("%.0f/s", perSecond)
		buf.WriteString(colorizeLabel(i, centerText(rateText, columnWidth)))
	}
	buf.WriteString(colorizeFrame("│\n"))
	lines++

	buf.WriteString(colorizeFrame("└"))
	buf.WriteString(colorizeFrame(strings.Repeat("─", totalWidth)))
	buf.WriteString(colorizeFrame("┘\n"))
	lines++

	return &buf, lines
}

type snapshot struct {
	name  string
	lines uint64
	bytes uint64
}

func snapshotMetrics(metrics []*runnerMetrics) []snapshot {
	out := make([]snapshot, len(metrics))
	for i, metric := range metrics {
		out[i] = snapshot{
			name:  metric.name,
			lines: metric.lines.Load(),
			bytes: metric.bytes.Load(),
		}
	}
	return out
}

func computeRanks(snapshots []snapshot) []int {
	type pair struct {
		idx   int
		lines uint64
	}
	pairs := make([]pair, len(snapshots))
	for i, snap := range snapshots {
		pairs[i] = pair{idx: i, lines: snap.lines}
	}
	sort.Slice(pairs, func(i, j int) bool {
		if pairs[i].lines == pairs[j].lines {
			return pairs[i].idx < pairs[j].idx
		}
		return pairs[i].lines > pairs[j].lines
	})
	ranks := make([]int, len(snapshots))
	rank := 1
	for _, pair := range pairs {
		ranks[pair.idx] = rank
		rank++
	}
	return ranks
}

func computeScale(maxLines, maxCap uint64) (uint64, int) {
	rows := uint64(maxChartRows)
	if rows == 0 {
		rows = 1
	}

	target := maxLines
	if maxCap > 0 {
		target = maxCap
	}
	if target == 0 {
		target = initialBlockSize * rows
	}

	minStep := target / rows
	if target%rows != 0 {
		minStep++
	}
	if minStep == 0 {
		minStep = 1
	}

	block := ((minStep + initialBlockSize - 1) / initialBlockSize) * initialBlockSize
	if block == 0 {
		block = initialBlockSize
	}

	return block, int(rows)
}

func renderBarCell(column int, value, lower, upper uint64) string {
	if value <= lower || upper <= lower {
		return strings.Repeat(" ", columnWidth)
	}
	glyph := blockGlyphForFraction(float64(value-lower) / float64(upper-lower))
	bar := strings.Repeat(glyph, barWidth)
	left := strings.Repeat(" ", barLeftPad)
	right := strings.Repeat(" ", barRightPad)
	return left + colorizeBar(column, bar) + right
}

func blockGlyphForFraction(fraction float64) string {
	blockGlyphs := []rune{'▁', '▂', '▃', '▄', '▅', '▆', '▇', '█'}

	if fraction <= 0 {
		return " "
	}
	if fraction >= 1 {
		return string(blockGlyphs[len(blockGlyphs)-1])
	}
	steps := float64(len(blockGlyphs))
	index := maxInt(int(math.Ceil(fraction*steps))-1, 0)
	if index >= len(blockGlyphs) {
		index = len(blockGlyphs) - 1
	}
	return string(blockGlyphs[index])
}

func centerText(text string, width int) string {
	if visualWidth(text) >= width {
		return truncateToWidth(text, width)
	}
	padding := width - visualWidth(text)
	left := padding / 2
	right := padding - left
	return strings.Repeat(" ", left) + text + strings.Repeat(" ", right)
}

func visualWidth(text string) int {
	width := 0
	for i := 0; i < len(text); {
		if next, ok := skipANSI(text, i); ok {
			i = next
			continue
		}
		_, size := utf8.DecodeRuneInString(text[i:])
		width++
		i += size
	}
	return width
}

func truncateToWidth(text string, limit int) string {
	if visualWidth(text) <= limit {
		return text
	}
	var builder strings.Builder
	width := 0
	for i := 0; i < len(text); {
		if next, ok := skipANSI(text, i); ok {
			builder.WriteString(text[i:next])
			i = next
			continue
		}
		r, size := utf8.DecodeRuneInString(text[i:])
		if width+1 > limit {
			break
		}
		builder.WriteRune(r)
		width++
		i += size
	}
	return builder.String()
}

func padRight(text string, width int) string {
	current := visualWidth(text)
	if current >= width {
		return truncateToWidth(text, width)
	}
	return text + strings.Repeat(" ", width-current)
}

func skipANSI(text string, i int) (int, bool) {
	if text[i] == '\x1b' && i+1 < len(text) && text[i+1] == '[' {
		j := i + 2
		for j < len(text) && text[j] != 'm' {
			j++
		}
		if j < len(text) {
			return j + 1, true
		}
	}
	return i, false
}

func formatNumber(value uint64) string {
	switch {
	case value >= 1_000_000_000:
		return fmt.Sprintf("%.1fB", float64(value)/1_000_000_000)
	case value >= 1_000_000:
		return fmt.Sprintf("%.1fM", float64(value)/1_000_000)
	case value >= 10_000:
		return fmt.Sprintf("%.0fk", float64(value)/1_000)
	case value >= 1_000:
		return fmt.Sprintf("%.1fk", float64(value)/1_000)
	default:
		return fmt.Sprintf("%d", value)
	}
}

func formatDuration(duration time.Duration) string {
	seconds := duration.Seconds()
	if seconds >= 1 {
		return fmt.Sprintf("%.1fs", seconds)
	}
	return fmt.Sprintf("%dms", duration.Milliseconds())
}

func printSummary(metrics []*runnerMetrics, runDuration time.Duration) {
	type summaryEntry struct {
		name  string
		lines uint64
	}
	entries := make([]summaryEntry, len(metrics))
	for i, metric := range metrics {
		entries[i] = summaryEntry{name: metric.name, lines: metric.lines.Load()}
	}
	sort.Slice(entries, func(i, j int) bool {
		if entries[i].lines == entries[j].lines {
			return entries[i].name < entries[j].name
		}
		return entries[i].lines > entries[j].lines
	})
	fmt.Printf("Results after %s per logger:\n", runDuration.Round(10*time.Millisecond))
	for i, entry := range entries {
		perSecond := float64(entry.lines) / runDuration.Seconds()
		column := loggerColumnIndex(entry.name)
		rank := colorizeRank(column, fmt.Sprintf("%d.", i+1))
		name := colorizeLabel(column, entry.name)
		padding := padSpaces(entry.name, 20)
		fmt.Printf("  %s %s%s%10d lines  (%6.0f lines/s)\n", rank, name, padding, entry.lines, perSecond)
	}
}

func loggerColumnIndex(name string) int {
	for idx, current := range loggerOrder {
		if current == name {
			return idx
		}
	}
	return 0
}

func padSpaces(text string, total int) string {
	width := visualWidth(text)
	if width >= total {
		return ""
	}
	return strings.Repeat(" ", total-width)
}

func diagramColorFor(column int) diagramColor {
	if len(diagramPalette) == 0 {
		return diagramColor{}
	}
	return toneDiagramColor(diagramPalette[(column/2)%len(diagramPalette)], column%2 == 1)
}

func toneDiagramColor(base diagramColor, brighten bool) diagramColor {
	return diagramColor{
		Bar:      toneANSI256(base.Bar, brighten),
		Label:    toneANSI256(base.Label, brighten),
		RankBold: toneANSI256(base.RankBold, brighten),
	}
}

func toneANSI256(code string, brighten bool) string {
	colorCode, bold, ok := parseANSI256(code)
	if !ok {
		return code
	}
	colorCode = adjustANSI256(colorCode, brighten)
	if bold {
		return fmt.Sprintf("\x1b[1;38;5;%dm", colorCode)
	}
	return fmt.Sprintf("\x1b[38;5;%dm", colorCode)
}

func parseANSI256(code string) (int, bool, bool) {
	var colorCode int

	if _, err := fmt.Sscanf(code, "\x1b[1;38;5;%dm", &colorCode); err == nil {
		return colorCode, true, true
	}
	if _, err := fmt.Sscanf(code, "\x1b[38;5;%dm", &colorCode); err == nil {
		return colorCode, false, true
	}
	return 0, false, false
}

func adjustANSI256(code int, brighten bool) int {
	if code >= 232 && code <= 255 {
		if brighten {
			return minInt(code+3, 255)
		}
		return maxInt(code-3, 232)
	}
	if code >= 16 && code <= 231 {
		index := code - 16
		r := index / 36
		g := (index / 6) % 6
		b := index % 6
		if brighten {
			r = minInt(r+1, 5)
			g = minInt(g+1, 5)
			b = minInt(b+1, 5)
		} else {
			r = maxInt(r-1, 0)
			g = maxInt(g-1, 0)
			b = maxInt(b-1, 0)
		}
		return 16 + (36 * r) + (6 * g) + b
	}
	return code
}

func colorizeBar(column int, text string) string {
	color := diagramColorFor(column)
	if color.Bar == "" {
		return text
	}
	return color.Bar + text + ansi.Reset
}

func colorizeLabel(column int, text string) string {
	color := diagramColorFor(column)
	if color.Label == "" {
		return text
	}
	return color.Label + text + ansi.Reset
}

func colorizeRank(column int, text string) string {
	color := diagramColorFor(column)
	if color.RankBold == "" {
		return text
	}
	return color.RankBold + text + ansi.Reset
}

func colorizeFrame(text string) string {
	if len(diagramPalette) == 0 || diagramPalette[0].Bar == "" {
		return text
	}
	return diagramPalette[0].Bar + text + ansi.Reset
}

func colorizeAxis(text string) string {
	if len(diagramPalette) == 0 || diagramPalette[0].Label == "" {
		return text
	}
	return diagramPalette[0].Label + text + ansi.Reset
}

func diagramPaletteFor(name string) []diagramColor {
	switch strings.ToLower(name) {
	case "solarized", "solarized-nightfall":
		return []diagramColor{
			{Bar: "\x1b[38;5;37m", Label: "\x1b[38;5;109m", RankBold: "\x1b[1;38;5;229m"},
			{Bar: "\x1b[38;5;136m", Label: "\x1b[38;5;178m", RankBold: "\x1b[1;38;5;230m"},
			{Bar: "\x1b[38;5;51m", Label: "\x1b[38;5;44m", RankBold: "\x1b[1;38;5;123m"},
			{Bar: "\x1b[38;5;166m", Label: "\x1b[38;5;208m", RankBold: "\x1b[1;38;5;214m"},
			{Bar: "\x1b[38;5;108m", Label: "\x1b[38;5;150m", RankBold: "\x1b[1;38;5;152m"},
		}
	case "catppuccin", "catppuccin-mocha":
		return []diagramColor{
			{Bar: "\x1b[38;5;217m", Label: "\x1b[38;5;183m", RankBold: "\x1b[1;38;5;223m"},
			{Bar: "\x1b[38;5;147m", Label: "\x1b[38;5;110m", RankBold: "\x1b[1;38;5;152m"},
			{Bar: "\x1b[38;5;109m", Label: "\x1b[38;5;150m", RankBold: "\x1b[1;38;5;189m"},
			{Bar: "\x1b[38;5;216m", Label: "\x1b[38;5;182m", RankBold: "\x1b[1;38;5;223m"},
			{Bar: "\x1b[38;5;205m", Label: "\x1b[38;5;204m", RankBold: "\x1b[1;38;5;218m"},
		}
	case "dracula":
		return []diagramColor{
			{Bar: "\x1b[38;5;219m", Label: "\x1b[38;5;147m", RankBold: "\x1b[1;38;5;225m"},
			{Bar: "\x1b[38;5;81m", Label: "\x1b[38;5;111m", RankBold: "\x1b[1;38;5;117m"},
			{Bar: "\x1b[38;5;204m", Label: "\x1b[38;5;170m", RankBold: "\x1b[1;38;5;218m"},
			{Bar: "\x1b[38;5;141m", Label: "\x1b[38;5;111m", RankBold: "\x1b[1;38;5;189m"},
			{Bar: "\x1b[38;5;177m", Label: "\x1b[38;5;171m", RankBold: "\x1b[1;38;5;183m"},
		}
	case "gruvboxlight", "gruvbox-light":
		return []diagramColor{
			{Bar: "\x1b[38;5;130m", Label: "\x1b[38;5;136m", RankBold: "\x1b[1;38;5;173m"},
			{Bar: "\x1b[38;5;108m", Label: "\x1b[38;5;107m", RankBold: "\x1b[1;38;5;150m"},
			{Bar: "\x1b[38;5;66m", Label: "\x1b[38;5;73m", RankBold: "\x1b[1;38;5;109m"},
			{Bar: "\x1b[38;5;142m", Label: "\x1b[38;5;142m", RankBold: "\x1b[1;38;5;179m"},
			{Bar: "\x1b[38;5;167m", Label: "\x1b[38;5;167m", RankBold: "\x1b[1;38;5;203m"},
		}
	case "gruvbox":
		return []diagramColor{
			{Bar: "\x1b[38;5;214m", Label: "\x1b[38;5;214m", RankBold: "\x1b[1;38;5;208m"},
			{Bar: "\x1b[38;5;142m", Label: "\x1b[38;5;142m", RankBold: "\x1b[1;38;5;190m"},
			{Bar: "\x1b[38;5;108m", Label: "\x1b[38;5;108m", RankBold: "\x1b[1;38;5;150m"},
			{Bar: "\x1b[38;5;172m", Label: "\x1b[38;5;109m", RankBold: "\x1b[1;38;5;214m"},
			{Bar: "\x1b[38;5;167m", Label: "\x1b[38;5;167m", RankBold: "\x1b[1;38;5;203m"},
		}
	case "tokyo", "tokyo-night":
		return []diagramColor{
			{Bar: "\x1b[38;5;69m", Label: "\x1b[38;5;74m", RankBold: "\x1b[1;38;5;111m"},
			{Bar: "\x1b[38;5;110m", Label: "\x1b[38;5;110m", RankBold: "\x1b[1;38;5;218m"},
			{Bar: "\x1b[38;5;176m", Label: "\x1b[38;5;173m", RankBold: "\x1b[1;38;5;219m"},
			{Bar: "\x1b[38;5;117m", Label: "\x1b[38;5;117m", RankBold: "\x1b[1;38;5;123m"},
			{Bar: "\x1b[38;5;210m", Label: "\x1b[38;5;210m", RankBold: "\x1b[1;38;5;213m"},
		}
	case "outrun":
		return []diagramColor{
			{Bar: "\x1b[38;5;201m", Label: "\x1b[38;5;219m", RankBold: "\x1b[1;38;5;225m"},
			{Bar: "\x1b[38;5;51m", Label: "\x1b[38;5;45m", RankBold: "\x1b[1;38;5;51m"},
			{Bar: "\x1b[38;5;99m", Label: "\x1b[38;5;135m", RankBold: "\x1b[1;38;5;201m"},
			{Bar: "\x1b[38;5;207m", Label: "\x1b[38;5;213m", RankBold: "\x1b[1;38;5;219m"},
			{Bar: "\x1b[38;5;39m", Label: "\x1b[38;5;81m", RankBold: "\x1b[1;38;5;45m"},
		}
	case "synthwave84", "synthwave":
		return []diagramColor{
			{Bar: "\x1b[38;5;201m", Label: "\x1b[38;5;213m", RankBold: "\x1b[1;38;5;219m"},
			{Bar: "\x1b[38;5;51m", Label: "\x1b[38;5;45m", RankBold: "\x1b[1;38;5;81m"},
			{Bar: "\x1b[38;5;135m", Label: "\x1b[38;5;135m", RankBold: "\x1b[1;38;5;207m"},
			{Bar: "\x1b[38;5;99m", Label: "\x1b[38;5;99m", RankBold: "\x1b[1;38;5;135m"},
			{Bar: "\x1b[38;5;45m", Label: "\x1b[38;5;51m", RankBold: "\x1b[1;38;5;51m"},
		}
	case "nord":
		return []diagramColor{
			{Bar: "\x1b[38;5;117m", Label: "\x1b[38;5;111m", RankBold: "\x1b[1;38;5;189m"},
			{Bar: "\x1b[38;5;74m", Label: "\x1b[38;5;74m", RankBold: "\x1b[1;38;5;189m"},
			{Bar: "\x1b[38;5;109m", Label: "\x1b[38;5;109m", RankBold: "\x1b[1;38;5;195m"},
			{Bar: "\x1b[38;5;103m", Label: "\x1b[38;5;103m", RankBold: "\x1b[1;38;5;153m"},
			{Bar: "\x1b[38;5;152m", Label: "\x1b[38;5;152m", RankBold: "\x1b[1;38;5;195m"},
		}
	case "material":
		return []diagramColor{
			{Bar: "\x1b[38;5;75m", Label: "\x1b[38;5;75m", RankBold: "\x1b[1;38;5;123m"},
			{Bar: "\x1b[38;5;110m", Label: "\x1b[38;5;110m", RankBold: "\x1b[1;38;5;189m"},
			{Bar: "\x1b[38;5;150m", Label: "\x1b[38;5;150m", RankBold: "\x1b[1;38;5;194m"},
			{Bar: "\x1b[38;5;180m", Label: "\x1b[38;5;180m", RankBold: "\x1b[1;38;5;223m"},
			{Bar: "\x1b[38;5;204m", Label: "\x1b[38;5;204m", RankBold: "\x1b[1;38;5;213m"},
		}
	case "everforest":
		return []diagramColor{
			{Bar: "\x1b[38;5;143m", Label: "\x1b[38;5;143m", RankBold: "\x1b[1;38;5;179m"},
			{Bar: "\x1b[38;5;107m", Label: "\x1b[38;5;107m", RankBold: "\x1b[1;38;5;150m"},
			{Bar: "\x1b[38;5;65m", Label: "\x1b[38;5;65m", RankBold: "\x1b[1;38;5;108m"},
			{Bar: "\x1b[38;5;179m", Label: "\x1b[38;5;179m", RankBold: "\x1b[1;38;5;221m"},
			{Bar: "\x1b[38;5;138m", Label: "\x1b[38;5;138m", RankBold: "\x1b[1;38;5;222m"},
		}
	case "one-dark", "one-dark-pro":
		return []diagramColor{
			{Bar: "\x1b[38;5;80m", Label: "\x1b[38;5;80m", RankBold: "\x1b[1;38;5;123m"},
			{Bar: "\x1b[38;5;110m", Label: "\x1b[38;5;110m", RankBold: "\x1b[1;38;5;189m"},
			{Bar: "\x1b[38;5;180m", Label: "\x1b[38;5;180m", RankBold: "\x1b[1;38;5;223m"},
			{Bar: "\x1b[38;5;173m", Label: "\x1b[38;5;173m", RankBold: "\x1b[1;38;5;219m"},
			{Bar: "\x1b[38;5;202m", Label: "\x1b[38;5;202m", RankBold: "\x1b[1;38;5;208m"},
		}
	case "monokai", "monokai-vibrant", "default":
		fallthrough
	default:
		return []diagramColor{
			{Bar: "\x1b[38;5;229m", Label: "\x1b[38;5;229m", RankBold: "\x1b[1;38;5;229m"},
			{Bar: "\x1b[38;5;141m", Label: "\x1b[38;5;141m", RankBold: "\x1b[1;38;5;189m"},
			{Bar: "\x1b[38;5;118m", Label: "\x1b[38;5;118m", RankBold: "\x1b[1;38;5;118m"},
			{Bar: "\x1b[38;5;198m", Label: "\x1b[38;5;198m", RankBold: "\x1b[1;38;5;205m"},
			{Bar: "\x1b[38;5;215m", Label: "\x1b[38;5;215m", RankBold: "\x1b[1;38;5;221m"},
		}
	}
}

func minDuration(a, b time.Duration) time.Duration {
	if a < b {
		return a
	}
	return b
}

func maxInt(a, b int) int {
	if a > b {
		return a
	}
	return b
}

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}
