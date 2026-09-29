module IntSet = Set.Make (Int)
module PowerSet (BaseSet : Set.S)
    (SetOrd : functor (S : Set.S) -> Set.OrderedType) =
  Set.Make (SetOrd (BaseSet))
module IntSetSet = PowerSet (IntSet)
  (functor (S : Set.S) -> struct type t = int
    let compare = compare end)
