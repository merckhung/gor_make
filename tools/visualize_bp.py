#!/usr/bin/env python3
"""
Visualize build target relationships from gor_make JSON output.

Supports all formats: Android.bp, Android.mk, BUILD.gn, CMake, Makefile, SCons.

Usage:
    gor_make --bp --json > bp.json
    python3 visualize_bp.py bp.json [output_prefix]

    gor_make --gn --json > gn.json
    python3 visualize_bp.py gn.json gn_project

    gor_make --cmake --json > cmake.json
    python3 visualize_bp.py cmake.json renderdoc

    gor_make --scons --json > scons.json
    python3 visualize_bp.py scons.json gem5

Produces:
    - <prefix>_dependency_graph.png  : Full dependency graph (matplotlib)
    - <prefix>_summary.txt           : Text summary of relationships
    - <prefix>_stats.png             : Statistics charts
"""

import json
import sys
import os
from collections import defaultdict, Counter
from io import StringIO

import matplotlib
matplotlib.use('Agg')  # Non-interactive backend
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np


# Color scheme for module types
TYPE_COLORS = {
    'cc_binary': '#4CAF50',           # Green
    'cc_library': '#2196F3',          # Blue
    'cc_library_shared': '#2196F3',   # Blue
    'cc_library_static': '#9C27B0',   # Purple
    'cc_library_headers': '#FF9800',  # Orange
    'cc_test': '#F44336',             # Red
    'cc_test_binary': '#F44336',      # Red
    'cc_benchmark': '#795548',        # Brown
    'cc_fuzz': '#E91E63',            # Pink
    'cc_genrule': '#607D8B',         # Blue Grey
    'genrule': '#607D8B',            # Blue Grey
    'rust_library': '#8BC34A',       # Light Green
    'rust_binary': '#CDDC39',        # Lime
    'rust_test': '#FFC107',          # Amber
    'sh_binary_host': '#00BCD4',     # Cyan
    'filegroup': '#BDBDBD',          # Grey
    'cc_defaults': '#BDBDBD',        # Grey
}

DEFAULT_COLOR = '#9E9E9E'  # Grey for unknown types


def load_json(filepath):
    """Load the JSON file produced by gor_make --bp --json."""
    with open(filepath) as f:
        return json.load(f)


def build_dependency_graph(data):
    """Build a dependency graph from the JSON data.

    Supports all gor_make JSON formats (Android.bp, Android.mk, BUILD.gn,
    CMake, Makefile, SCons).

    Returns:
        nodes: dict of target_name -> {type, src_dir, srcs, ...}
        edges: list of (from_target, to_target, dep_type)
    """
    nodes = {}
    edges = []

    # Android.bp uses "modules", others use "targets"
    items = data.get('modules', []) or data.get('targets', [])

    for item in items:
        name = item['name']
        nodes[name] = item

        # Android.bp dependency types
        for dep in item.get('shared_libs', []):
            edges.append((name, dep, 'shared'))
        for dep in item.get('static_libs', []):
            edges.append((name, dep, 'static'))
        for dep in item.get('whole_static_libs', []):
            edges.append((name, dep, 'whole_static'))
        for dep in item.get('header_libs', []):
            edges.append((name, dep, 'header'))

        # BUILD.gn dependency types
        for dep in item.get('deps', []):
            edges.append((name, dep, 'dep'))
        for dep in item.get('public_deps', []):
            edges.append((name, dep, 'public_dep'))

        # CMake dependency types
        for dep in item.get('link_libs', []):
            edges.append((name, dep, 'link'))

        # Makefile dependency types
        for dep in item.get('prerequisites', []):
            edges.append((name, dep, 'prereq'))

    return nodes, edges


def compute_graph_layout(nodes, edges, max_nodes=80):
    """Compute a hierarchical layout for the dependency graph.

    Places modules with no dependents at the top (leaves of the dependency tree)
    and modules with many dependents at the bottom (roots/foundational libs).

    Uses a simple level-based algorithm:
    Level 0 = modules with no outgoing deps (leaf consumers)
    Level N = modules whose deps are all in levels < N
    """
    # Only include nodes that are in our set (skip external/system libs)
    node_set = set(nodes.keys())
    
    # If too many nodes, filter to only those with dependencies
    if len(node_set) > max_nodes:
        connected = set()
        for src, dst, _ in edges:
            if src in node_set:
                connected.add(src)
            if dst in node_set:
                connected.add(dst)
        node_set = connected

    # Build adjacency: deps[node] = set of nodes it depends on (within our set)
    deps = defaultdict(set)
    dependents = defaultdict(set)
    for src, dst, dep_type in edges:
        if src in node_set and dst in node_set:
            deps[src].add(dst)
            dependents[dst].add(src)

    # Compute levels using iterative approach
    levels = {}
    remaining = set(node_set)

    while remaining:
        progress = False
        for node in list(remaining):
            # A node's level is max(level of all its deps) + 1
            node_deps = deps[node]
            if all(d in levels for d in node_deps):
                if node_deps:
                    levels[node] = max(levels[d] for d in node_deps) + 1
                else:
                    levels[node] = 0
                remaining.discard(node)
                progress = True

        if not progress:
            # Circular dependency or external dep — assign remaining to level 0
            for node in remaining:
                levels[node] = 0
            break

    # Group nodes by level
    max_level = max(levels.values()) if levels else 0
    level_nodes = defaultdict(list)
    for node, level in levels.items():
        level_nodes[level].append(node)

    # Assign x, y coordinates
    positions = {}
    node_list = list(levels.keys())

    for level in range(max_level + 1):
        nodes_at_level = sorted(level_nodes.get(level, []))
        n = len(nodes_at_level)
        for i, node in enumerate(nodes_at_level):
            x = (i + 0.5) / max(n, 1)
            y = 1.0 - level / max(max_level, 1)
            positions[node] = (x, y)

    return positions, edges, node_set


def draw_dependency_graph(nodes, edges, positions, node_set, filepath):
    """Draw the dependency graph using matplotlib."""
    fig, ax = plt.subplots(1, 1, figsize=(24, 16))
    ax.set_xlim(-0.05, 1.05)
    ax.set_ylim(-0.05, 1.05)
    ax.set_aspect('auto')
    ax.set_facecolor('#FAFAFA')
    ax.set_title('Target Dependency Graph', fontsize=20, fontweight='bold', pad=20)
    ax.axis('off')

    # Draw edges
    for src, dst, dep_type in edges:
        if src not in positions or dst not in positions:
            continue
        x1, y1 = positions[src]
        x2, y2 = positions[dst]

        # Color by dependency type
        dep_colors = {
            'shared': '#2196F3',
            'static': '#9C27B0',
            'whole_static': '#E91E63',
            'header': '#FF9800',
        }
        color = dep_colors.get(dep_type, '#BDBDBD')
        alpha = 0.3 if dep_type == 'shared' else 0.5

        ax.annotate('',
                     xy=(x2, y2), xytext=(x1, y1),
                     arrowprops=dict(arrowstyle='->', color=color,
                                     alpha=alpha, lw=0.8,
                                     connectionstyle='arc3,rad=0.05'))

    # Draw nodes
    for name, (x, y) in positions.items():
        if name not in nodes:
            continue
        mod_type = nodes[name].get('type', 'unknown')
        color = TYPE_COLORS.get(mod_type, DEFAULT_COLOR)

        # Node circle
        circle = plt.Circle((x, y), 0.012, color=color, zorder=5, ec='#333333', lw=0.5)
        ax.add_patch(circle)

        # Node label (full target name)
        label = name
        ax.text(x, y - 0.018, label, fontsize=5.5, ha='center', va='top',
                fontfamily='monospace', alpha=0.9, fontweight='bold')

    # Legend
    legend_items = [
        mpatches.Patch(color='#4CAF50', label='cc_binary'),
        mpatches.Patch(color='#2196F3', label='cc_library (shared)'),
        mpatches.Patch(color='#9C27B0', label='cc_library_static'),
        mpatches.Patch(color='#F44336', label='cc_test'),
        mpatches.Patch(color='#795548', label='cc_benchmark'),
        mpatches.Patch(color='#E91E63', label='cc_fuzz'),
        mpatches.Patch(color='#8BC34A', label='rust_library'),
        mpatches.Patch(color='#607D8B', label='genrule'),
    ]
    # Filter legend to only types present
    present_types = set(nodes[n].get('type', '') for n in node_set if n in nodes)
    legend_items = [item for item in legend_items
                    if any(t in present_types for t in [item.get_label()])]

    # Edge legend
    edge_legend = [
        mpatches.Patch(color='#2196F3', alpha=0.5, label='shared_libs'),
        mpatches.Patch(color='#9C27B0', alpha=0.7, label='static_libs'),
        mpatches.Patch(color='#E91E63', alpha=0.7, label='whole_static_libs'),
        mpatches.Patch(color='#FF9800', alpha=0.7, label='header_libs'),
    ]

    leg1 = ax.legend(handles=legend_items, loc='upper left', fontsize=8,
                     title='Module Types', title_fontsize=9,
                     framealpha=0.9, edgecolor='#333333')
    ax.add_artist(leg1)
    ax.legend(handles=edge_legend, loc='lower left', fontsize=8,
              title='Dependency Types', title_fontsize=9,
              framealpha=0.9, edgecolor='#333333')

    plt.tight_layout()
    fig.savefig(filepath, dpi=200, bbox_inches='tight',
                facecolor='white', edgecolor='none')
    plt.close(fig)
    print(f'  Dependency graph saved to: {filepath}')


def draw_statistics(nodes, edges, filepath):
    """Draw statistics charts."""
    fig, axes = plt.subplots(2, 2, figsize=(16, 12))
    fig.suptitle('Target Statistics', fontsize=18, fontweight='bold')

    # 1. Module type distribution
    ax = axes[0, 0]
    type_counts = Counter(n.get('type', 'unknown') for n in nodes.values())
    top_types = type_counts.most_common(12)
    types, counts = zip(*top_types) if top_types else ([], [])
    colors = [TYPE_COLORS.get(t, DEFAULT_COLOR) for t in types]
    bars = ax.barh(range(len(types)), counts, color=colors, edgecolor='#333', lw=0.5)
    ax.set_yticks(range(len(types)))
    ax.set_yticklabels(types, fontsize=9)
    ax.set_xlabel('Count')
    ax.set_title('Module Type Distribution', fontsize=12, fontweight='bold')
    ax.invert_yaxis()
    for bar, count in zip(bars, counts):
        ax.text(bar.get_width() + 0.1, bar.get_y() + bar.get_height()/2,
                str(count), va='center', fontsize=8)

    # 2. Top depended-upon modules
    ax = axes[0, 1]
    dep_count = Counter()
    for src, dst, _ in edges:
        dep_count[dst] += 1
    top_deps = dep_count.most_common(15)
    dep_names, dep_counts = zip(*top_deps) if top_deps else ([], [])
    ax.barh(range(len(dep_names)), dep_counts, color='#FF5722', edgecolor='#333', lw=0.5)
    ax.set_yticks(range(len(dep_names)))
    ax.set_yticklabels(dep_names, fontsize=8)
    ax.set_xlabel('Number of Dependents')
    ax.set_title('Most Depended-Upon Modules', fontsize=12, fontweight='bold')
    ax.invert_yaxis()

    # 3. Modules with most dependencies
    ax = axes[1, 0]
    out_dep_count = Counter()
    for src, dst, _ in edges:
        out_dep_count[src] += 1
    top_out = out_dep_count.most_common(15)
    out_names, out_counts = zip(*top_out) if top_out else ([], [])
    ax.barh(range(len(out_names)), out_counts, color='#3F51B5', edgecolor='#333', lw=0.5)
    ax.set_yticks(range(len(out_names)))
    ax.set_yticklabels(out_names, fontsize=8)
    ax.set_xlabel('Number of Dependencies')
    ax.set_title('Modules with Most Dependencies', fontsize=12, fontweight='bold')
    ax.invert_yaxis()

    # 4. Source file count distribution
    ax = axes[1, 1]
    src_counts = [len(n.get('srcs', [])) for n in nodes.values() if n.get('srcs')]
    if src_counts:
        ax.hist(src_counts, bins=range(0, max(src_counts) + 2, 1),
                color='#009688', edgecolor='#333', lw=0.5, alpha=0.8)
        ax.set_xlabel('Number of Source Files')
        ax.set_ylabel('Number of Modules')
        ax.set_title('Source File Count Distribution', fontsize=12, fontweight='bold')
    else:
        ax.text(0.5, 0.5, 'No source files', ha='center', va='center',
                transform=ax.transAxes)

    plt.tight_layout()
    fig.savefig(filepath, dpi=150, bbox_inches='tight',
                facecolor='white', edgecolor='none')
    plt.close(fig)
    print(f'  Statistics chart saved to: {filepath}')


def write_summary(nodes, edges, filepath):
    """Write a text summary of the module relationships."""
    # Try to detect format from the node types
    with open(filepath, 'w') as f:
        f.write('=' * 70 + '\n')
        f.write('Target Relationship Summary\n')
        f.write('=' * 70 + '\n\n')

        # Basic stats
        f.write(f'Total targets: {len(nodes)}\n')
        f.write(f'Total dependency edges: {len(edges)}\n\n')

        # Type distribution
        type_counts = Counter(n.get('type', 'unknown') for n in nodes.values())
        f.write('Target Type Distribution:\n')
        f.write('-' * 40 + '\n')
        for t, c in type_counts.most_common():
            f.write(f'  {t:40s} {c:5d}\n')

        # Top depended-upon
        dep_count = Counter()
        for src, dst, _ in edges:
            dep_count[dst] += 1
        f.write(f'\nTop 20 Most Depended-Upon Modules:\n')
        f.write('-' * 60 + '\n')
        for name, count in dep_count.most_common(20):
            mod_type = nodes.get(name, {}).get('type', 'external')
            f.write(f'  {name:45s} ({mod_type:20s}) <- {count:3d} modules\n')

        # Top consumers
        out_dep_count = Counter()
        for src, dst, _ in edges:
            out_dep_count[src] += 1
        f.write(f'\nTop 20 Modules with Most Dependencies:\n')
        f.write('-' * 60 + '\n')
        for name, count in out_dep_count.most_common(20):
            mod_type = nodes.get(name, {}).get('type', 'unknown')
            f.write(f'  {name:45s} ({mod_type:20s}) -> {count:3d} deps\n')

        # Dependency chains
        f.write(f'\nDependency Chains (max depth 5):\n')
        f.write('-' * 60 + '\n')

        deps_map = defaultdict(list)
        for src, dst, dep_type in edges:
            deps_map[src].append((dst, dep_type))

        def trace_chain(node, depth=0, visited=None):
            if visited is None:
                visited = set()
            if node in visited or depth > 5:
                return
            visited.add(node)
            node_deps = deps_map.get(node, [])
            if not node_deps:
                return
            for dst, dep_type in node_deps[:3]:  # Limit to first 3 deps
                indent = '  ' * (depth + 1)
                dep_label = {'shared': 'shared', 'static': 'static',
                             'whole_static': 'whole', 'header': 'header'}.get(dep_type, dep_type)
                f.write(f'{indent}{node} --[{dep_label}]--> {dst}\n')
                trace_chain(dst, depth + 1, visited.copy())
            visited.discard(node)

        # Trace from binaries (top-level consumers)
        binaries = [n for n, m in nodes.items()
                     if m.get('type') in ('cc_binary', 'rust_binary', 'cc_test')]
        for name in sorted(binaries)[:10]:
            if deps_map.get(name):
                f.write(f'\n  Chain from {name}:\n')
                trace_chain(name)

        # External dependencies (deps not in our module set)
        external_deps = set()
        for src, dst, _ in edges:
            if dst not in nodes:
                external_deps.add(dst)
        if external_deps:
            f.write(f'\nExternal Dependencies ({len(external_deps)} unique):\n')
            f.write('-' * 60 + '\n')
            for dep in sorted(external_deps)[:30]:
                count = sum(1 for _, d, _ in edges if d == dep)
                f.write(f'  {dep:45s} (used by {count} modules)\n')
            if len(external_deps) > 30:
                f.write(f'  ... and {len(external_deps) - 30} more\n')

    print(f'  Summary saved to: {filepath}')


def main():
    if len(sys.argv) < 2:
        print(f'Usage: {sys.argv[0]} <bp.json> [output_prefix]')
        print(f'Example: {sys.argv[0]} /tmp/aosp_bp.json aosp_system_extras')
        sys.exit(1)

    json_file = sys.argv[1]
    prefix = sys.argv[2] if len(sys.argv) > 2 else 'bp'

    print(f'Loading {json_file}...')
    data = load_json(json_file)

    count = data.get("module_count") or data.get("target_count") or 0
    print(f'Building dependency graph from {count} targets...')
    nodes, edges = build_dependency_graph(data)

    output_dir = os.path.dirname(json_file) or '.'

    print(f'\nGenerating outputs (prefix: {prefix}):')

    # Dependency graph
    print('Computing graph layout...')
    positions, filtered_edges, node_set = compute_graph_layout(nodes, edges)
    draw_dependency_graph(nodes, edges, positions, node_set,
                         os.path.join(output_dir, f'{prefix}_dependency_graph.png'))

    # Statistics
    draw_statistics(nodes, edges,
                    os.path.join(output_dir, f'{prefix}_stats.png'))

    # Text summary
    write_summary(nodes, edges,
                  os.path.join(output_dir, f'{prefix}_summary.txt'))

    print(f'\nDone! Generated 3 files with prefix "{prefix}".')


if __name__ == '__main__':
    main()
