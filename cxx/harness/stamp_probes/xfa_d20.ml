module IntSet = Set.Make (Int)
module PowerSet (BaseSet : Set.S)
    (SetOrd : functor (S : Set.S) -> Set.OrderedType) =
  Set.Make (SetOrd (BaseSet))
module Id (S : Set.S) = S
let x = let module N = PowerSet (IntSet) (Id) in
  N.cardinal N.empty
