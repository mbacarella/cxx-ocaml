module IntSet = Set.Make (Int)
module PowerSet (BaseSet : Set.S)
    (SetOrd : functor (S : Set.S) -> Set.OrderedType) =
  Set.Make (SetOrd (BaseSet))
module P1 = PowerSet (IntSet)
module P2 = PowerSet (IntSet)
module I1 = P1 (functor (S : Set.S) -> S)
module I2 = P2 (functor (S : Set.S) -> S)
