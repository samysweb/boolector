(set-option :incremental false)
(set-logic QF_BV)
(declare-fun s () (_ BitVec 8))
(declare-fun t () (_ BitVec 8))

(assert (not (= (bvxor s t) (bvor (bvand s (bvnot t)) (bvand (bvnot s) t)))))



