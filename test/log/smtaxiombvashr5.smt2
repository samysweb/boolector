(set-option :incremental false)
(set-logic QF_BV)
(declare-fun s () (_ BitVec 5))
(declare-fun t () (_ BitVec 5))

(assert (not (= (bvashr s t) (ite (= ((_ extract 4 4) s) (_ bv0 1)) (bvlshr s t) (bvnot (bvlshr (bvnot s) t))))))



