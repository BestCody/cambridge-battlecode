// Tree-ensemble evaluator for models exported by training/export_cpp.py.
// A model is a flat node array; each tree starts at roots[t] and adds its leaf value to the margin of
// class group groups[t]. Splits follow XGBoost exactly: go left when x < threshold, and take the
// learned default branch when x is NaN (a feature the bot could not observe this turn).
#pragma once
#include <cmath>
#include <cstdint>

namespace pdi {

struct Node {
    std::int16_t feature;       // -1 for a leaf
    std::uint8_t default_left;  // branch for a missing (NaN) feature
    float value;                // split threshold, or the leaf value
    std::int32_t left, right;
};

struct Model {
    const Node* nodes;
    const std::int32_t* roots;
    const std::uint8_t* groups;
    int n_trees;
    const float* base;          // initial margin per group
    int n_groups;               // 1 for binary models
    const std::int16_t* classes;  // head class id for each output (binary: {0, 1})
    int n_classes;
    const char* const* class_names;
    int binary;
};

// Raw margins, one per group.
inline void margins(const Model& m, const float* x, float* out) {
    for (int g = 0; g < m.n_groups; ++g)
        out[g] = m.base[g];
    for (int t = 0; t < m.n_trees; ++t) {
        const Node* n = &m.nodes[m.roots[t]];
        while (n->feature >= 0) {
            const float v = x[n->feature];
            const bool left = std::isnan(v) ? n->default_left : v < n->value;
            n = &m.nodes[left ? n->left : n->right];
        }
        out[m.groups[t]] += n->value;
    }
}

// Class probabilities (softmax, or the logistic for binary models) into probs[n_classes].
inline void probabilities(const Model& m, const float* x, float* probs) {
    float z[128];
    margins(m, x, z);
    if (m.binary) {
        probs[1] = 1.0f / (1.0f + std::exp(-z[0]));
        probs[0] = 1.0f - probs[1];
        return;
    }
    float mx = z[0];
    for (int g = 1; g < m.n_groups; ++g)
        mx = z[g] > mx ? z[g] : mx;
    float sum = 0;
    for (int g = 0; g < m.n_groups; ++g)
        sum += probs[g] = std::exp(z[g] - mx);
    for (int g = 0; g < m.n_groups; ++g)
        probs[g] /= sum;
}

// Most likely head class id, optionally restricted by allowed(class_id) (e.g. only legal moves).
template <class Allowed>
inline int best_class(const Model& m, const float* x, Allowed allowed, float* p_out = nullptr) {
    float p[128];
    probabilities(m, x, p);
    int best = -1;
    for (int k = 0; k < m.n_classes; ++k)
        if (allowed(m.classes[k]) && (best < 0 || p[k] > p[best]))
            best = k;
    if (p_out)
        *p_out = best >= 0 ? p[best] : 0.0f;
    return best >= 0 ? m.classes[best] : -1;
}

inline int best_class(const Model& m, const float* x) {
    return best_class(m, x, [](int) { return true; });
}

// Binary heads: probability of "yes".
inline float yes_probability(const Model& m, const float* x) {
    float p[2];
    probabilities(m, x, p);
    return p[1];
}

} // namespace pdi
