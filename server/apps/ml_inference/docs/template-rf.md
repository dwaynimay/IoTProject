# Template — RandomForest Pipeline

Gunakan template ini ketika user memilih RandomForest sebagai classifier.

**Kapan pakai RF vs SVM:**
- Dataset besar (>20K windows) → RF lebih cepat
- Butuh feature importance bawaan → RF
- Data tidak perlu normalisasi ketat → RF (StandardScaler tetap bagus, tapi opsional)
- Dataset kecil (<5K), margin keputusan kompleks → SVM lebih baik

---

## Import Tambahan

```python
from sklearn.ensemble import RandomForestClassifier
from sklearn.inspection import permutation_importance
# Tidak perlu ImbPipeline jika tidak pakai SMOTE
from sklearn.pipeline import Pipeline
```

## Pipeline Definition

```python
# RF tidak butuh SMOTE jika pakai class_weight='balanced'
# Gunakan sklearn Pipeline biasa (bukan ImbPipeline)
pipeline_template = Pipeline([
    ('scaler', StandardScaler()),   # opsional untuk RF, tapi bagus untuk konsistensi
    ('clf',    RandomForestClassifier(
                   class_weight='balanced',
                   random_state=42,
                   n_jobs=-1
                   # probability=True TIDAK DIPERLUKAN — RF selalu punya predict_proba
               ))
])
```

## GridSearchCV

```python
param_grid = {
    'clf__n_estimators'   : [100, 200, 300],
    'clf__max_depth'      : [None, 10, 20, 30],
    'clf__min_samples_split': [2, 5, 10],
    'clf__max_features'   : ['sqrt', 'log2'],
}

grid_search = GridSearchCV(
    pipeline_template, param_grid,
    cv=StratifiedKFold(n_splits=5, shuffle=True, random_state=42),
    scoring='f1_macro', n_jobs=-1, verbose=1
)
grid_search.fit(X_train, y_train)
best_model = grid_search.best_estimator_
```

## Feature Importance (Bonus — RF punya ini bawaan)

```python
# Bawaan RF (impurity-based) — cepat tapi bisa bias untuk fitur high-cardinality
rf_step  = best_model.named_steps['clf']
imp_mean = rf_step.feature_importances_
top_idx  = np.argsort(imp_mean)[::-1][:20]
feat_names = [name for name, _ in FEATURE_SCHEMA]

fig, ax = plt.subplots(figsize=(10, 6))
ax.barh([feat_names[i] for i in top_idx[::-1]], imp_mean[top_idx[::-1]], color='#1565C0', alpha=0.85)
ax.set_title('Feature Importance (Impurity-based)', fontweight='bold')
ax.set_xlabel('Mean decrease in impurity')
ax.grid(axis='x', alpha=0.3)
plt.tight_layout()
plt.savefig('/content/feature_importance.png', dpi=130)
plt.show()
```

## Handling Imbalance dengan RF

RF sudah punya `class_weight='balanced'` yang cukup untuk imbalance ringan-sedang.
Untuk imbalance berat (>5x), tambahkan SMOTE dengan ImbPipeline:

```python
from imblearn.pipeline import Pipeline as ImbPipeline
from imblearn.over_sampling import SMOTE

pipeline_with_smote = ImbPipeline([
    ('scaler', StandardScaler()),
    ('smote',  SMOTE(random_state=42, k_neighbors=3)),
    ('clf',    RandomForestClassifier(class_weight='balanced', random_state=42, n_jobs=-1))
])
# param_grid sama, prefix 'clf__'
```

## Verifikasi Compatibility

RF tidak perlu `probability=True` — sudah ada secara default. Tapi tetap jalankan Step 9 standard:

```python
# RF: classes_ ada di named_steps['clf']
clf_step    = best_model.named_steps['clf']
clf_classes = list(clf_step.classes_)
assert clf_classes == CLASSES
print(f'✓ RF classes_: {clf_classes}')
```
