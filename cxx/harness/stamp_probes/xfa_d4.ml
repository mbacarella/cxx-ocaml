module IntSet = Set.Make (Int)
module PowerSet (BaseSet : Set.S)
    (SetOrd : functor (S : Set.S) -> Set.OrderedType) =
  Set.Make (SetOrd (BaseSet))
module P1 = PowerSet (IntSet)
module IntSetSet = P1 (functor (S : Set.S) -> S)
