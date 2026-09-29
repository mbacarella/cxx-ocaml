module IntSet = Set.Make (Int)
module PowerSet (BaseSet : Set.S)
    (SetOrd : functor (S : Set.S) -> Set.OrderedType) =
  Set.Make (SetOrd (BaseSet))
module IntSetSet = PowerSet (IntSet)
