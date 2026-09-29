module IntSet = Set.Make (Int)
module PowerSet (BaseSet : Set.S)
    (SetOrd : functor (S : Set.S) -> Set.OrderedType) =
  Set.Make (SetOrd (BaseSet))
let x = let module N = PowerSet (IntSet) (functor (S : Set.S) -> S) in
  N.cardinal N.empty
