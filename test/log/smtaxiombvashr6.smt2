(set-option :incremental false)
(set-logic QF_BV)
(declare-fun s () (_ BitVec 6))
(declare-fun t () (_ BitVec 6))

(assert (not (= (bvashr s t) (ite (= ((_ extract 5 5) s) (_ bv0 1)) (bvlshr s t) (bvnot (bvlshr (bvnot s) t))))))



