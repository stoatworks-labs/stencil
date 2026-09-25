/**
 * Stencil — the cutter, `source/Bridge.cpp`, ported to JavaScript.
 *
 * **This is a port, and only a reader checks it.** It was compared once
 * against the C++ (AGENTS.md, "The browser demo"): a scratch driver ran
 * Bridge.cpp and node ran this file on the same grids with the same exact
 * flood, frame after frame with last frame's bridges carried, and every bridge
 * and every cell of every cut grid was compared. Nothing re-runs that.
 *
 * What it is, from Bridge.h:
 *
 *   The GRID is the stencil on its lattice with a ring of one cell all round,
 *   the MARGIN, which is sheet: a piece of sheet is held when it is
 *   4-connected to the margin. Sheet is 4-connected and hole 8-connected.
 *
 *   Drop: floating pieces of fewer cells than `minIslandArea` are filled with
 *   hole (they fall out and are painted).
 *
 *   A pass labels the sheet by RUNS with union-find (the smaller root wins, so
 *   a piece's label is its first cell in raster order, plus one), asks the
 *   FLOOD for every cell's nearest sheet cell of another piece, and gives every
 *   floating piece the shortest bridge from any of its cells, ties to the
 *   lowest (a.y, a.x, b.y, b.x). Last frame's bridges are kept -- at the cell
 *   of the same piece nearest where they were, within `keepRadius` of either
 *   end -- while no more than `keepSlack` longer than the shortest; kept ones
 *   are cut first. Pieces already joined this pass are skipped. The band is
 *   every cell within width / 2 of the segment, never thinner than
 *   |n.x| + |n.y| (4-connected at any angle). Passes until nothing floats.
 *
 * The flood is a callback, as in the C++: the page answers it with the
 * plugin's own jump-flood shaders in WebGL2 (plugin.js), the comparison with an
 * exact brute-force flood.
 *
 *   flood(labels: Uint32Array, width, height, region {x0, y0, x1, y1},
 *         second: Uint32Array | null) -> Uint32Array | null
 *
 * writes, for every cell of the region, the nearest sheet cell whose label
 * differs from that cell's own nearest, packed x | y << 16, or kNone; it is
 * handed last pass's array (or null) and returns the one it wrote, keeping
 * what was outside the region, as the C++'s `std::vector::resize` does.
 *
 * Not ported: the Perturb test hooks (all 0 in the plugin), the timing and the
 * per-pass snapshots, which only the harness reads.
 */

export const kHole = 0;
export const kSheet = 1;
export const kBridge = 2;
export const kNone = 0xffffffff;

export const isSheet = (c) => c !== kHole;
export const pack = (x, y) => ((x | (y << 16)) >>> 0);

//------------------------------------------------------------ union-find

/// Union-find with path halving, union by index (the smaller root wins).
class Forest {
  constructor() {
    this.parent = new Int32Array(0);
  }

  reset(n) {
    if (this.parent.length < n) this.parent = new Int32Array(Math.max(n, this.parent.length * 2));
    for (let i = 0; i < n; i += 1) this.parent[i] = i;
  }

  find(i) {
    const parent = this.parent;
    while (parent[i] !== i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  }

  unite(a, b) {
    a = this.find(a);
    b = this.find(b);
    if (a === b) return;
    if (a < b) this.parent[b] = a;
    else this.parent[a] = b;
  }
}

//------------------------------------------------------------ run labelling

/// The sheet by runs: each row's maximal stretches of sheet are the nodes, and
/// a run is united with every run of the row below whose x range overlaps.
class Runs {
  constructor() {
    this.count = 0;
    this.row = new Int32Array(1024);
    this.x0 = new Int32Array(1024);
    this.x1 = new Int32Array(1024); // inclusive
    this.rowStart = new Int32Array(0);
    this.forest = new Forest();
  }

  push(row, x0, x1) {
    if (this.count === this.row.length) {
      const grow = (a) => {
        const b = new Int32Array(a.length * 2);
        b.set(a);
        return b;
      };
      this.row = grow(this.row);
      this.x0 = grow(this.x0);
      this.x1 = grow(this.x1);
    }
    this.row[this.count] = row;
    this.x0[this.count] = x0;
    this.x1[this.count] = x1;
    this.count += 1;
  }

  build(grid) {
    const { width, height, cells } = grid;
    this.count = 0;
    if (this.rowStart.length !== height + 1) this.rowStart = new Int32Array(height + 1);
    for (let y = 0; y < height; y += 1) {
      this.rowStart[y] = this.count;
      const base = y * width;
      let x = 0;
      while (x < width) {
        while (x < width && cells[base + x] === kHole) x += 1;
        if (x >= width) break;
        const x0 = x;
        while (x < width && cells[base + x] !== kHole) x += 1;
        this.push(y, x0, x - 1);
      }
    }
    this.rowStart[height] = this.count;

    const forest = this.forest;
    forest.reset(this.count);
    for (let y = 1; y < height; y += 1) {
      let below = this.rowStart[y - 1];
      const end = this.rowStart[y];
      for (let r = this.rowStart[y]; r < this.rowStart[y + 1]; r += 1) {
        while (below < end && this.x1[below] < this.x0[r]) below += 1;
        for (let b = below; b < end && this.x0[b] <= this.x1[r]; b += 1) forest.unite(r, b);
      }
    }
  }

  /// Every cell's label (0 for hole); returns the margin's label (the anchor).
  fill(grid, labels) {
    labels.fill(0);
    const w = grid.width;
    for (let r = 0; r < this.count; r += 1) {
      const root = this.forest.find(r);
      const id = (this.row[root] * w + this.x0[root] + 1) >>> 0;
      const start = this.row[r] * w + this.x0[r];
      labels.fill(id, start, start + (this.x1[r] - this.x0[r] + 1));
    }
    return labels.length === 0 ? 0 : labels[0];
  }
}

/// Label the sheet 4-connected: every sheet cell's piece as root + 1, 0 for
/// hole. Returns { labels, anchor }.
export function Label(grid) {
  const runs = new Runs();
  runs.build(grid);
  const labels = new Uint32Array(grid.width * grid.height);
  const anchor = runs.fill(grid, labels);
  return { labels, anchor };
}

//------------------------------------------------------------ the band

/// The cells a bridge from a to b of `width` cells covers.
export function Band(ax, ay, bx, by, width, gridWidth, gridHeight, out) {
  out.length = 0;
  const dx = bx - ax;
  const dy = by - ay;
  const length = Math.sqrt(dx * dx + dy * dy);
  if (length <= 0) return out;
  const ux = dx / length;
  const uy = dy / length;
  const nx = -uy;
  const ny = ux;
  let full = Math.max(0, width);
  full = Math.max(full, Math.abs(nx) + Math.abs(ny));
  const half = 0.5 * full;
  const eps = 1e-9;

  const x0 = Math.max(0, Math.floor(Math.min(ax, bx) - half - 1));
  const x1 = Math.min(gridWidth - 1, Math.ceil(Math.max(ax, bx) + half + 1));
  const y0 = Math.max(0, Math.floor(Math.min(ay, by) - half - 1));
  const y1 = Math.min(gridHeight - 1, Math.ceil(Math.max(ay, by) + half + 1));
  for (let y = y0; y <= y1; y += 1) {
    for (let x = x0; x <= x1; x += 1) {
      const px = x - ax;
      const py = y - ay;
      const t = px * ux + py * uy;
      const s = px * nx + py * ny;
      if (t >= -eps && t <= length + eps && Math.abs(s) <= half + eps) out.push(x, y);
    }
  }
  return out;
}

//------------------------------------------------------------ the cutter

const INVALID = Number.POSITIVE_INFINITY;

/**
 * Drop the small islands and bridge the rest, in place.
 *
 * @param {{width:number, height:number, cells:Uint8Array}} grid  margin included, row 0 at the bottom
 * @param {{width:number, minIslandArea:number, keepSlack:number, keepRadius:number, maxPasses?:number}} settings
 * @param {Array<{ax:number, ay:number, bx:number, by:number}>} history  last frame's bridges, in this grid's cells
 * @param {Function} flood  see the top of this file
 * @returns {{passes:number, islands:number, dropped:number, floatingLeft:number, bridges:Array}}
 */
export function Cut(grid, settings, history, flood) {
  const result = { passes: 0, islands: 0, dropped: 0, floatingLeft: 0, bridges: [] };
  const w = grid.width;
  const h = grid.height;
  const cellsN = grid.cells.length;
  const cells = grid.cells;
  if (w < 3 || h < 3 || cellsN !== w * h) return result;
  const maxPasses = settings.maxPasses ?? 32;

  // The margin is sheet, whatever the caller left there.
  for (let x = 0; x < w; x += 1) {
    cells[x] = kSheet;
    cells[(h - 1) * w + x] = kSheet;
  }
  for (let y = 0; y < h; y += 1) {
    cells[y * w] = kSheet;
    cells[y * w + w - 1] = kSheet;
  }

  const labels = new Uint32Array(cellsN);
  let anchor = 0;
  const runs = new Runs();
  const relabel = () => {
    runs.build(grid);
    anchor = runs.fill(grid, labels);
  };
  relabel();

  //--- drop the specks ------------------------------------------------------
  {
    const n = runs.count;
    const size = new Float64Array(n);
    for (let r = 0; r < n; r += 1) size[runs.forest.find(r)] += runs.x1[r] - runs.x0[r] + 1;
    const held = runs.forest.find(0);
    const drop = new Uint8Array(n);
    let any = false;
    for (let r = 0; r < n; r += 1) {
      if (runs.forest.find(r) !== r || r === held) continue;
      if (size[r] < settings.minIslandArea) {
        result.dropped += 1;
        drop[r] = 1;
        any = true;
      } else {
        result.islands += 1;
      }
    }
    if (any) {
      for (let r = 0; r < n; r += 1) {
        if (drop[runs.forest.find(r)]) {
          const start = runs.row[r] * w + runs.x0[r];
          cells.fill(kHole, start, start + (runs.x1[r] - runs.x0[r] + 1));
        }
      }
      relabel();
    }
  }

  let second = null;
  const band = [];
  const dense = new Int32Array(cellsN).fill(-1);
  let roots = [];
  const joined = new Forest();

  // The best candidate of each floating piece, and its kept one, as parallel
  // arrays: (length2, ay, ax, by, bx), length2 = Infinity for none.
  let bL = new Float64Array(0);
  let bAy = new Int32Array(0);
  let bAx = new Int32Array(0);
  let bBy = new Int32Array(0);
  let bBx = new Int32Array(0);
  let kL = new Float64Array(0);
  let kAy = new Int32Array(0);
  let kAx = new Int32Array(0);
  let kBy = new Int32Array(0);
  let kBx = new Int32Array(0);
  let kMoved = new Float64Array(0);

  for (let pass = 1; pass <= maxPasses; pass += 1) {
    if (pass > 1) relabel();
    for (const l of roots) dense[l - 1] = -1;
    roots = [];
    for (let r = 0; r < runs.count; r += 1) {
      if (runs.forest.find(r) !== r) continue;
      const l = (runs.row[r] * w + runs.x0[r] + 1) >>> 0;
      if (l === anchor) continue;
      dense[l - 1] = roots.length;
      roots.push(l);
    }
    if (roots.length === 0) break;
    result.passes = pass;

    const n = roots.length;
    if (bL.length < n) {
      const m = Math.max(n, bL.length * 2);
      bL = new Float64Array(m); bAy = new Int32Array(m); bAx = new Int32Array(m); bBy = new Int32Array(m); bBx = new Int32Array(m);
      kL = new Float64Array(m); kAy = new Int32Array(m); kAx = new Int32Array(m); kBy = new Int32Array(m); kBx = new Int32Array(m);
      kMoved = new Float64Array(m);
    }
    bL.fill(INVALID, 0, n);
    kL.fill(INVALID, 0, n);
    kMoved.fill(0, 0, n);

    //--- the region -----------------------------------------------------------
    let region = { x0: 0, y0: 0, x1: w, y1: h };
    if (pass > 1 && second !== null && second.length === cellsN) {
      const x0 = new Int32Array(n).fill(w);
      const y0 = new Int32Array(n).fill(h);
      const x1 = new Int32Array(n).fill(-1);
      const y1 = new Int32Array(n).fill(-1);
      const reach = new Float64Array(n).fill(INVALID);
      for (let y = 1; y < h - 1; y += 1) {
        for (let x = 1; x < w - 1; x += 1) {
          const i = y * w + x;
          const li = labels[i];
          if (li === 0) continue;
          const d = dense[li - 1];
          if (d < 0) continue;
          if (x < x0[d]) x0[d] = x;
          if (y < y0[d]) y0[d] = y;
          if (x > x1[d]) x1[d] = x;
          if (y > y1[d]) y1[d] = y;
          const packed = second[i];
          if (packed === kNone) continue;
          const sx = packed & 0xffff;
          const sy = packed >>> 16;
          if (sx >= w || sy >= h) continue;
          const other = labels[sy * w + sx];
          if (other === 0 || other === li) continue;
          const dx = sx - x;
          const dy = sy - y;
          const l2 = dx * dx + dy * dy;
          if (l2 < reach[d]) reach[d] = l2;
        }
      }
      let bounded = n > 0;
      const box = { x0: w, y0: h, x1: 0, y1: 0 };
      for (let d = 0; d < n && bounded; d += 1) {
        if (reach[d] === INVALID) {
          bounded = false;
          break;
        }
        const grow = Math.ceil(Math.sqrt(reach[d])) + 1;
        box.x0 = Math.min(box.x0, Math.max(0, x0[d] - grow));
        box.y0 = Math.min(box.y0, Math.max(0, y0[d] - grow));
        box.x1 = Math.max(box.x1, Math.min(w, x1[d] + grow + 1));
        box.y1 = Math.max(box.y1, Math.min(h, y1[d] + grow + 1));
      }
      if (bounded) region = box;
    }

    second = flood(labels, w, h, region, second);
    if (second === null || second.length !== cellsN) {
      result.floatingLeft = -1;
      break;
    }

    //--- the candidates ---------------------------------------------------------
    for (let y = 1; y < h - 1; y += 1) {
      for (let x = 1; x < w - 1; x += 1) {
        const i = y * w + x;
        const li = labels[i];
        if (li === 0) continue;
        const d = dense[li - 1];
        if (d < 0) continue;
        const packed = second[i];
        if (packed === kNone) continue;
        const sx = packed & 0xffff;
        const sy = packed >>> 16;
        const dx = sx - x;
        const dy = sy - y;
        const l2 = dx * dx + dy * dy;
        // Key order: length2, ay, ax, by, bx.
        if (l2 < bL[d] || (l2 === bL[d] && (y < bAy[d] || (y === bAy[d] && (x < bAx[d] || (x === bAx[d] && (sy < bBy[d] || (sy === bBy[d] && sx < bBx[d])))))))) {
          bL[d] = l2; bAy[d] = y; bAx[d] = x; bBy[d] = sy; bBx[d] = sx;
        }
      }
    }

    // Last frame's bridges: within keepRadius of either end, the cell of the
    // same piece nearest that end whose bridge is no more than keepSlack
    // longer than the shortest.
    for (const old of history) {
      for (let end = 0; end < 2; end += 1) {
        const ox = end === 0 ? old.ax : old.bx;
        const oy = end === 0 ? old.ay : old.by;
        const rad = settings.keepRadius;
        for (let y = Math.max(1, oy - rad); y <= Math.min(h - 2, oy + rad); y += 1) {
          for (let x = Math.max(1, ox - rad); x <= Math.min(w - 2, ox + rad); x += 1) {
            const i = y * w + x;
            const li = labels[i];
            if (li === 0) continue;
            const d = dense[li - 1];
            if (d < 0 || bL[d] === INVALID) continue;
            if (x < region.x0 || y < region.y0 || x >= region.x1 || y >= region.y1) continue;
            const packed = second[i];
            if (packed === kNone) continue;
            const sx = packed & 0xffff;
            const sy = packed >>> 16;
            const dx = sx - x;
            const dy = sy - y;
            const l2 = dx * dx + dy * dy;
            if (Math.sqrt(l2) > Math.sqrt(bL[d]) + settings.keepSlack + 1e-9) continue;
            const mx = x - ox;
            const my = y - oy;
            const moved = mx * mx + my * my;
            const valid = kL[d] !== INVALID;
            const less = l2 < kL[d] || (l2 === kL[d] && (y < kAy[d] || (y === kAy[d] && (x < kAx[d] || (x === kAx[d] && (sy < kBy[d] || (sy === kBy[d] && sx < kBx[d])))))));
            if (!valid || moved < kMoved[d] || (moved === kMoved[d] && less)) {
              kL[d] = l2; kAy[d] = y; kAx[d] = x; kBy[d] = sy; kBx[d] = sx;
              kMoved[d] = moved;
            }
          }
        }
      }
    }

    //--- choose, in order, and cut ---------------------------------------------
    const choices = [];
    for (let d = 0; d < n; d += 1) {
      if (bL[d] === INVALID) continue;
      if (kL[d] !== INVALID) choices.push({ l2: kL[d], ay: kAy[d], ax: kAx[d], by: kBy[d], bx: kBx[d], island: roots[d], kept: true });
      else choices.push({ l2: bL[d], ay: bAy[d], ax: bAx[d], by: bBy[d], bx: bBx[d], island: roots[d], kept: false });
    }
    // Kept first; then the key. The keys are distinct (a is a cell of its own
    // piece), so this is a total order and std::sort's instability never shows.
    choices.sort((a, b) => {
      if (a.kept !== b.kept) return a.kept ? -1 : 1;
      return a.l2 - b.l2 || a.ay - b.ay || a.ax - b.ax || a.by - b.by || a.bx - b.bx;
    });

    const margin = n;
    joined.reset(n + 1);
    const idOf = (l) => (l === anchor ? margin : dense[l - 1]);
    for (const c of choices) {
      const la = labels[c.ay * w + c.ax];
      const lb = labels[c.by * w + c.bx];
      if (la === 0 || lb === 0) continue;
      const ia = idOf(la);
      const ib = idOf(lb);
      if (joined.find(ia) === joined.find(ib)) continue;
      joined.unite(ia, ib);
      Band(c.ax, c.ay, c.bx, c.by, settings.width, w, h, band);
      for (let k = 0; k < band.length; k += 2) {
        const x = band[k];
        const y = band[k + 1];
        const at = y * w + x;
        if (cells[at] === kHole) cells[at] = kBridge;
        // Whatever piece the band touches, it joins.
        const nxs = [x, x - 1, x + 1, x, x];
        const nys = [y, y, y, y - 1, y + 1];
        for (let q = 0; q < 5; q += 1) {
          const qx = nxs[q];
          const qy = nys[q];
          if (qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
          const l = labels[qy * w + qx];
          if (l !== 0) joined.unite(ia, idOf(l));
        }
      }
      result.bridges.push({ ax: c.ax, ay: c.ay, bx: c.bx, by: c.by, pass, island: c.island, length: Math.sqrt(c.l2), kept: c.kept });
    }
  }

  //--- what is left floating (nothing, unless the passes ran out) ---------------
  relabel();
  if (result.floatingLeft >= 0) {
    const held = runs.forest.find(0);
    for (let r = 0; r < runs.count; r += 1) {
      if (runs.forest.find(r) === r && held !== r) result.floatingLeft += 1;
    }
  }
  return result;
}
