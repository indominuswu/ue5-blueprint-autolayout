#Blueprint Graph Auto - Layout Specification

## Terms

* **exec flow**: control flow composed of connections between exec pins
* **degree**: number of exec in/out pins
* **junction**: branch/merge point where exec in/out degree is not (1,1) (only exec is counted)
* **data node**: node with no exec pins (data-only)
* **lane**: exec path connecting junction -> junction, or a path with a missing
  junction endpoint (including intermediate nodes)
* **L**: set of lanes
* **lane members**: ordered nodes traversed in a lane (exec order, including non-exec nodes connected to each exec)
* **laneIndex**: final ordering index of lanes (0..|L|-1)
* **real node**: actual `UEdGraphNode` (not dummy/synthetic)
* **dummy**: synthetic node for long-edge splitting, etc. (do not write back)
* **NodeKey / PinKey**: stable keys for determinism (see definitions below)
* **port-aware barycenter**: barycenter ordering that includes pin position offsets
* **|S|**: cardinality of set `S`
* **rank**: X-direction layer index (left-to-right column)
* **order**: Y-direction row index (top-to-bottom order)
* **minLen(u,v)**: minimum required rank gap for edge `u -> v` (`rank[v] >= rank[u] + minLen(u,v)`, default 1)
* **maxLen(u,v)**: maximum allowed rank gap for edge `u -> v` (`rank[v] <= rank[u] + maxLen(u,v)`, default +inf)
* **weight**: constraint weight in a difference-constraints edge `a -> b` meaning `rank[b] >= rank[a] + weight`

## 0. Objective

Deterministically reorder Blueprint nodes while prioritizing edge crossing reduction and readability.

## 1. Overview

Reorder nodes by the following steps.

1. Generate the layout of lanes and junctions with the Sugiyama framework.
2. For each lane, generate the layout of lane members with the Sugiyama framework.

In the sections below, (1) is specified as "Exec Lane / Junction Minimal Layout Spec" and (2) as "Exec In-Lane Real Node Layout Minimal Spec".

### 1.1 Sugiyama Framework

A general framework for layering directed graphs to reduce crossings and assign coordinates.
It consists of the following four steps.

#### 1.1.1 Cycle Removal

If directed cycles exist, reverse some edges for layout to create a DAG.
Reversal only changes direction inside the layout computation;
actual graph connections are unchanged.

    Select edges to reverse deterministically; the minimal spec is:

1. Start DFS in NodeKey ascending order, and traverse outgoing edges in PinKey ascending order.
2. During DFS, list edges that go to the recursion stack as back-edges.
3. If any back-edge exists, reverse the single edge with the smallest tuple
   (NodeKey(out), PinKey(outPin), NodeKey(in), PinKey(in)), and repeat until no back-edges remain.

#### 1.1.2 Layer Assignment

Assign a rank to each node in the DAG so that edges go left-to-right.
Choose ranks within constraints such as minLen and maxLen.

The minimal spec uses earliest-feasible (longest-path) when maxLen is +inf.

1. Build the DAG using the effective directions after cycle removal.
2. Take indegree-0 nodes in NodeKey ascending order, and process outgoing edges in PinKey ascending order to get a deterministic topological order.
3. Initialize `rank[v] = 0`, then for each edge `u -> v` in order apply
   `rank[v] = max(rank[v], rank[u] + minLen(u,v))`.
4. The default `minLen` is 1 and default `maxLen` is +inf.

If any edge has finite `maxLen`, solve the difference constraints instead:

1. Build a constraint edge list in deterministic order:
   * forward: `u -> v` with weight `minLen(u,v)`
   * reverse: `v -> u` with weight `-maxLen(u,v)` (skip if `maxLen` is +inf)
   Use the topological order from step 2 for forward edges, and list reverse edges
   in the same order as their forward edge.
   Each constraint edge `(a -> b, weight w)` represents `rank[b] >= rank[a] + w`.
2. Initialize `rank[v] = 0`.
3. Repeat for at most |V|-1 iterations:
   * Forward pass: relax constraints in list order (forward edges, then reverse edges).
   * Backward pass: relax constraints in reverse list order.
   * If no update occurs in both passes, stop early.
4. If any update occurs on the |V|th iteration (in either pass), the constraints are infeasible.
5. If infeasible, run one final forward pass in list order and keep the result.


#### 1.1.3 Crossing Reduction

Reorder nodes between adjacent ranks to reduce edge crossings.
Run a fixed number of barycenter-based sweeps.

The minimal spec is:

1. Give each rank's nodes the current order `order` (0..n-1).
2. In a left-to-right sweep, enumerate the neighbor set `N(v)` in the left rank in PinKey ascending order, and compute
   `bc[v] = sum(order[u] + off(u, p_out_to_v) for u in N(v))`.
   Here `off(u, p_out_to_v) = pinIndex(u, p_out_to_v) / m(u)`,
   `pinIndex` is `PinIndexWithinOwner`, and `m(u)` is the number of exec output pins of `u` (range 0..1).
   If `N(v)` is empty, set `bc[v] = order[v]`.
   Sort by `bc` ascending, break ties by `NodeKey` ascending, and reassign `order` to 0..n-1.
3. The right-to-left sweep is the same but uses neighbors in the right rank.
4. Repeat steps 1-3 a fixed number of times (no early exit).

Assume long edges are split into dummy nodes so edges connect only adjacent ranks.

#### 1.1.4 Coordinate Assignment

Convert rank/row to actual X/Y coordinates with spacing.


## 2. Lane / Junction Layout

### 2.1 Input

#### 2.1.1 Required Data

* node set `V` (corresponds to UEdGraphNode)
* exec edge set `E_exec` (exec output pin -> exec input pin connection)
* stable keys for each node/pin

  * `NodeKey(node)` (defined later)
  * `PinKey(pin)` (defined later)

### 2.2 Determinism

#### 2.2.1 NodeKey (Stable)

`NodeKey(node)` must be stable within the same asset. Use `NodeGuid`.

#### 2.2.2 PinKey (Stable)

`PinKey(pin) = (NodeKey(owner), Direction, PinName, PinIndexWithinOwner)`

#### 2.2.3 Traversal Order (Required)

* Junction enumeration, lane enumeration, adjacency list enumeration, and all sorting must be stabilized by the keys above.
* The sweep count is fixed (no early exit).

### 2.3 Junction and Lane Definitions

#### 2.3.1 Junction

A node `n` is a junction iff:

* `n` has exec pins, and
* `ExecInDegree(n) != 1` **or** `ExecOutDegree(n) != 1`

(Degree is counted over `E_exec`.)

#### 2.3.2 Exec Output Pin Enumeration Order

Exec output pins of a junction are deterministically ordered by:

* Default: `PinIndexWithinOwner` ascending

#### 2.3.3 Lane (junction -> junction exec path, endpoints optional)

For each junction `Js` and each exec output pin `pOut`:

* Advance along exec edges until the next junction `Je` is reached, and form one lane `L`.
* `LaneStart(L) = (Js, pOut)`
* `LaneEnd(L) = (Je, pIn)` (the input exec pin of `Je` if known; otherwise `pIn = null`)

If no junction is reached, set `LaneEnd(L) = (null, null)`.

Note: If there are no intermediate nodes (a direct connection), do not create a lane.

If a connected component has no junctions, create a single lane for that
component. Use the NodeKey-smallest node as an anchor, define a synthetic
output pin named `Orphan` (index 0) for `LaneStart`, and set `LaneEnd = (null, null)`.

#### 2.3.4 LaneId (Stable)

`LaneId(L) = (NodeKey(LaneStart.node), PinKey(LaneStart.pin), NodeKey(Je or null), PinKey(pIn or null))`

### 2.4 Processing

The lane/junction layout is performed on a graph where junctions and lanes are treated as nodes.

1. Treat both lane and junction as nodes of width 1.
2. Treat a lane as a virtual node with exec in/out degree (1,1).
   If an endpoint is missing, set the degree of that side to 0.
3. Set `minLen = 1` for all edges.
4. Set `maxLen = 1` for edges incident to a lane node (otherwise +inf).
5. Run the Sugiyama steps up to crossing reduction (cycle removal -> layer assignment -> crossing reduction).
6. Do not assign coordinates at this stage; output only order (`rank` / `order`).


## 3. In-Lane Real Node Layout

### 3.1 Objective

For real nodes within a lane, determine only order (`rank` / `order`), and assign coordinates later.

### 3.2 Input

* `Members[]` for each lane (exec order)
* edge set `E_lane` used inside the lane

  * exec edges (adjacent in Members)
  * data edges within the lane
* stable keys (`NodeKey` / `PinKey`)

### 3.3 Processing

1. For each lane, build `G_lane = (V_lane, E_lane)`; `V_lane` includes only real members.
2. Set `minLen = 1` for all edges.
3. Set `maxLen = 1` for edges incident to a data node (otherwise +inf).
4. Apply 1.1.1-1.1.3 to `G_lane` to compute `rankInLane` and `orderInLane`.
5. Do not assign coordinates here.

### 3.4 Output

* Assign `rankInLane` and `orderInLane` to each real member.
* Do not write back dummy/synthetic nodes.


## 4. Global Rank/Order Composition (No Reordering)

Combine the lane/junction layout (blockRank/blockOrder) with the in-lane
layout (localRank/localOrder) to produce global ranks and orders without
reordering blocks.

### 4.1 Terms

* **block**: a lane or a junction
* **blockRank / blockOrder**: rank/order from the lane/junction layout
* **localRank / localOrder**: rank/order inside a lane (junction uses 0,0)
* **baseRank[block]**: leftmost global rank of a block
* **colBase[r]**: leftmost global rank of column r (blockRank = r)
* **laneSpan(L)**: `max(localRank) + 1` for lane L
* **blockId**: `LaneId` for lanes, `NodeKey` for junctions
* **minLenData**: minimum rank gap for data edges (default 1)

### 4.2 Local normalization

* junction: `localRank = 0`, `localOrder = 0`
* lane nodes: `localRank = rankInLane - min(rankInLane)` (0-based)
* `laneSpan(L) = max(localRank) + 1`

### 4.3 Difference constraints (no reordering)

Use difference constraints and compute the earliest-feasible ranks.

Column order:

* `colBase[r+1] >= colBase[r] + colGap` (default `colGap = 1`)

Column assignment:

* `baseRank[B] = colBase[blockRank[B]]` (two constraints with weight 0)

Exec skeleton:

* `baseRank[L] >= baseRank[Js] + 1`
* `baseRank[Je] >= baseRank[L] + laneSpan(L)`

Data edges across blocks:

For any data edge `u -> v` where `B(u) != B(v)`:

* `baseRank[B(v)] >= baseRank[B(u)] + (localRank(u) - localRank(v) + minLenData)`

### 4.4 Cycle removal and solving

* If the constraint graph contains cycles, apply cycle removal only to data edges
  (use the same deterministic reversal rule as 1.1.1).
* On the resulting DAG, compute longest-path (earliest-feasible) ranks
  for `colBase[]` and `baseRank[]`.

### 4.5 Global rank

* `globalRank[n] = baseRank[B(n)] + localRank[n]`

### 4.6 Global order (block concatenation, fixed)

For each global rank `r`:

1. Collect blocks that have at least one node with `globalRank = r`.
2. Sort blocks by `(blockOrder, blockId)`.
3. Within each block, sort nodes at rank `r` by `(localOrder, NodeKey)`.
4. Concatenate in order and assign `globalOrder = 0..`.


## 5. Lane/Junction Virtual Coordinate Assignment

Place the lane/junction graph in a virtual global coordinate system.
The node with `rank=0, order=0` (junction or lane) is placed at the origin.

### 5.1 Input

* nodes of the lane/junction graph (`rank`, `order`)
* node dimensions

  * junction: `width` / `height` of the real node
  * lane: `LaneWidthInLane` / `LaneHeightInLane` computed in 5.3
* spacing

  * `NodeSpacingX` (X direction)
  * `NodeSpacingY` (Y direction)

### 5.2 Virtual Global Coordinates (left-top basis)

`rank` is X and `order` is Y, and `order` is independent per rank.

* `RankWidthLaneGraph[r] = max(width(n) for n in rank r)`
* `RankXLeftLaneGraph[0] = 0`
* `RankXLeftLaneGraph[r] = RankXLeftLaneGraph[r-1] + RankWidthLaneGraph[r-1] + NodeSpacingX`
* `xTopLeftLaneGraph[n] = RankXLeftLaneGraph[r] + (RankWidthLaneGraph[r] - width(n)) / 2`

Sort nodes in rank `r` by `order` and pack from the top:

* first: `yTopLeftLaneGraph = 0`
* next: `yTopLeftLaneGraph[next] = yTopLeftLaneGraph[prev] + height(prev) + NodeSpacingY`

The node with `rank = 0` and `order = 0` is placed at `(0, 0)`.

### 5.3 Lane Bounding Box

* `RankWidthInLane[r] = max(width(n) for n in rank r)`
* `RankHeightInLane[r] = sum(height(n) for n in rank r) + NodeSpacingY * (count_r - 1)`
* `LaneWidthInLane = sum(RankWidthInLane[r]) + NodeSpacingX * (numRanks - 1)`
* `LaneHeightInLane = max(RankHeightInLane[r])`

### 5.4 Output

* junction: keep `(xTopLeftLaneGraph[n], yTopLeftLaneGraph[n])` as virtual global coordinates
* lane: keep `LaneTopLeft = (xTopLeftLaneGraph[n], yTopLeftLaneGraph[n])`


## 6. In-Lane Real Nodes (Virtual Coordinates)

Compute virtual global coordinates of nodes inside each lane.

### 6.1 Input

* `Members[]` for each lane (with `rankInLane` / `orderInLane`)
* `width` / `height` of each node
* spacing

  * `NodeSpacingX`
  * `NodeSpacingY`
* lane left-top coordinate `LaneTopLeft` (virtual, from 5.4)

### 6.2 In-Lane X Position (rank)

* `RankWidthInLane[r] = max(width(n) for n in rank r)` (nodes in rank r)
* `RankXLeftInLane[0] = 0`
* `RankXLeftInLane[r] = RankXLeftInLane[r-1] + RankWidthInLane[r-1] + NodeSpacingX`

Place each node `n` (rank r) at the column center:

* `xLocalTopLeftInLane[n] = RankXLeftInLane[r] + (RankWidthInLane[r] - width(n)) / 2`

### 6.3 In-Lane Y Position (order, independent per rank)

Sort nodes in rank r stably by `(orderInLane, NodeKey)` and pack from the top:

* first: `yLocalTopLeftInLane = 0`
* next: `yLocalTopLeftInLane[next] = yLocalTopLeftInLane[prev] + height(prev) + NodeSpacingY`

### 6.4 Conversion to Virtual Global Coordinates

* `xTopLeftInLane[n] = LaneTopLeft.X + xLocalTopLeftInLane[n]`
* `yTopLeftInLane[n] = LaneTopLeft.Y + yLocalTopLeftInLane[n]`


## 7. Virtual Coordinates -> Graph Coordinates

Translate the virtual global coordinate system into the graph coordinate system.

### 7.1 Input

* anchor old position `OldPos[a]` (`a` is the node with `rank=0, order=0`)

  * junction: pre-layout top-left position
  * lane: top-left position of the exec node in Members[] with the smallest `rankInLane` and `orderInLane`

### 7.2 Transform

* junction: `GraphPos[n] = (xTopLeftLaneGraph[n], yTopLeftLaneGraph[n]) + OldPos[a]`
* in-lane real nodes: `GraphPos[n] = (xTopLeftInLane[n], yTopLeftInLane[n]) + OldPos[a]`

### 7.3 Output

* All real nodes: place at `GraphPos`
