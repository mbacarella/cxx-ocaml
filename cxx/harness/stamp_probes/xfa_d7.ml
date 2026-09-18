module IntSet = Set.Make (Int)
module PowerSet (BaseSet : Set.S)
    (SetOrd : functor (S : Set.S) -> Set.OrderedType) =
  Set.Make (SetOrd (BaseSet))
module Id (S : Set.S) = S
module IntSetSet = PowerSet (IntSet) (Id)
module IntSetSet2 = PowerSet (IntSet) (Id)
