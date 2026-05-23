"""
wrappers.py — Sklearn-compatible wrappers untuk model .pkl non-standar.

Masalah:
    Beberapa model disimpan sebagai dict (bukan estimator sklearn langsung).
    Engine membutuhkan object dengan interface predict_proba(X) -> ndarray.
    Wrapper di sini menjembatani keduanya.

Kenapa file terpisah?
    pickle menyimpan MODULE PATH class saat dump.
    Saat load, Python harus bisa import class dari path yang sama.
    Karena itu wrapper HARUS berada di module yang stabil dan importable,
    bukan di script sementara / __main__.

Cara buat wrapper pkl:
    from apps.ml_inference.wrappers import SVMActivityWrapper
    wrapper = SVMActivityWrapper(pipeline, label_encoder)
    pickle.dump(wrapper, open("model_wrapper.pkl", "wb"), protocol=4)
"""

from __future__ import annotations

import numpy as np


class SVMActivityWrapper:
    """
    Wrapper sklearn-compatible untuk model SVM yang disimpan sebagai dict.

    Dict pkl format (fall_detection_svm.pkl):
        {
            "model"         : sklearn Pipeline (scaler -> smote -> svc),
            "label_encoder" : LabelEncoder (int -> str),
            "classes"       : list label str,
            "feature_names" : list nama fitur,
            "n_features"    : int,
            ...
        }

    Engine butuh interface:
        model.predict_proba(X: ndarray (n_samples, n_features)) -> ndarray (n_samples, n_classes)
        model.classes_    : array of str labels (urutan harus match config JSON)
        model.n_features_in_ : int

    Catatan SMOTE:
        SMOTE hanya dipakai saat training (oversampling minoritas).
        Saat inferensi, pipeline di-bypass — langsung scaler -> svm.
        Ini behavior sklearn standard: Pipeline.predict() sudah skip SMOTE di predict time.
        Tapi karena Pipeline.predict_proba() memanggil transform pada semua step kecuali
        yang bukan transformer di predict time, kita tetap extract hanya scaler+svm
        untuk kejelasan.
    """

    def __init__(self, pipeline, label_encoder) -> None:
        """
        Args:
            pipeline      : sklearn Pipeline dengan steps [scaler, smote, svm]
            label_encoder : LabelEncoder yang memetakan int -> str class name
        """
        steps = pipeline.steps
        self._scaler       = steps[0][1]   # StandardScaler
        self._svm          = steps[-1][1]  # SVC

        self._label_encoder    = label_encoder
        self.classes_          = np.array([str(c) for c in label_encoder.classes_])
        self.n_features_in_    = self._svm.n_features_in_

    def predict_proba(self, X: np.ndarray) -> np.ndarray:
        """
        Args:
            X: ndarray shape (n_samples, n_features), dtype float64

        Returns:
            ndarray shape (n_samples, n_classes) — probabilitas tiap kelas.
            Urutan kelas sesuai self.classes_.
        """
        X_scaled = self._scaler.transform(X)
        return self._svm.predict_proba(X_scaled)

    def predict(self, X: np.ndarray) -> np.ndarray:
        """Predict kelas (encoded integer)."""
        X_scaled = self._scaler.transform(X)
        return self._svm.predict(X_scaled)
