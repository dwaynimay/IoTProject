# Template — GradientBoosting & XGBoost Pipeline

Gunakan template ini ketika user memilih GBM atau XGBoost.

**Kapan pakai GBM/XGBoost:**
- Dataset menengah-besar (5K–500K windows)
- Butuh performa terbaik, siap tuning lebih lama
- Fitur campuran (stat + meta) → XGBoost bagus untuk ini
- Produksi dengan latensi ketat → XGBoost lebih cepat dari SVM untuk batch besar

---

## Option A — sklearn GradientBoostingClassifier

```python
from sklearn.ensemble import GradientBoostingClassifier
from sklearn.pipeline import Pipeline

# GBM tidak punya class_weight bawaan — perlu sample_weight atau SMOTE
pipeline_gbm = Pipeline([
    ('scaler', StandardScaler()),
    ('clf',    GradientBoostingClassifier(
                   n_estimators=200,
                   learning_rate=0.1,
                   max_depth=4,
                   random_state=42
                   # sudah punya predict_proba()
               ))
])

param_grid = {
    'clf__n_estimators' : [100, 200, 300],
    'clf__learning_rate': [0.05, 0.1, 0.2],
    'clf__max_depth'    : [3, 4, 5],
}
```

**Handling imbalance GBM** — tidak ada `class_weight`, gunakan SMOTE:

```python
from imblearn.pipeline import Pipeline as ImbPipeline
from imblearn.over_sampling import SMOTE

pipeline_gbm = ImbPipeline([
    ('scaler', StandardScaler()),
    ('smote',  SMOTE(random_state=42, k_neighbors=3)),
    ('clf',    GradientBoostingClassifier(random_state=42))
])
# param_grid: prefix 'clf__'
```

---

## Option B — XGBoost (Direkomendasikan untuk performa terbaik)

```python
!pip install xgboost -q
from xgboost import XGBClassifier
from sklearn.pipeline import Pipeline

# XGBoost perlu label 0, 1, 2, ... (integer berurutan) — LabelEncoder sudah handle ini
pipeline_xgb = Pipeline([
    ('scaler', StandardScaler()),   # opsional untuk XGB, tapi konsisten
    ('clf',    XGBClassifier(
                   objective='multi:softprob',   # output probabilitas
                   eval_metric='mlogloss',
                   use_label_encoder=False,
                   random_state=42,
                   n_jobs=-1,
                   # Handling imbalance: scale_pos_weight tidak berlaku untuk multi-class
                   # Gunakan sample_weight atau SMOTE
               ))
])

param_grid = {
    'clf__n_estimators'  : [100, 200, 300],
    'clf__max_depth'     : [3, 4, 6],
    'clf__learning_rate' : [0.05, 0.1, 0.2],
    'clf__subsample'     : [0.8, 1.0],
    'clf__colsample_bytree': [0.8, 1.0],
}
```

## Verifikasi Compatibility XGBoost

```python
# XGBoost: classes_ ada di named_steps['clf']
clf_step    = best_model.named_steps['clf']
clf_classes = list(clf_step.classes_)

# XGBoost classes_ mungkin berupa array int (0,1,2,3) bukan string
# Konversi jika perlu:
if clf_classes == list(range(len(CLASSES))):
    print(f'✓ XGB classes_ = integer index {clf_classes} → sesuai dengan CLASSES={CLASSES}')
else:
    assert clf_classes == CLASSES, f'✗ Mismatch: {clf_classes} vs {CLASSES}'
    print(f'✓ XGB classes_ konsisten: {clf_classes}')
```

> ⚠ **XGBoost dan urutan kelas:** XGBoost menyimpan `classes_` sebagai integer (0,1,2...) jika input `y` sudah integer dari LabelEncoder. Output `predict_proba()` tetap terurut sesuai integer tersebut, yang berkorespondensi dengan `CLASSES`. Ini sudah benar — config `"labels"` harus tetap `CLASSES` (string).

## LightGBM (alternatif XGBoost, lebih cepat untuk dataset besar)

```python
!pip install lightgbm -q
from lightgbm import LGBMClassifier

pipeline_lgbm = Pipeline([
    ('scaler', StandardScaler()),
    ('clf',    LGBMClassifier(
                   objective='multiclass',
                   class_weight='balanced',
                   random_state=42,
                   n_jobs=-1,
                   verbose=-1
               ))
])

param_grid = {
    'clf__n_estimators'  : [100, 200],
    'clf__max_depth'     : [-1, 10, 20],
    'clf__learning_rate' : [0.05, 0.1],
    'clf__num_leaves'    : [31, 63],
}
```
