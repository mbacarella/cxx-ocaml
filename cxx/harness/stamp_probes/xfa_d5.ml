module IntSet = Set.Make (Int)
module PowerSet (BaseSet : Set.S)
    (SetOrd : functor (S : Set.S) -> Set.OrderedType) =
  Set.Make (SetOrd (BaseSet))
module Id (S : Set.S) = S
module P1 = PowerSet (IntSet)
module IntSetSet = P1 (Id)
