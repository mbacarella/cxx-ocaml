module IntSet = Set.Make (Int)
module PowerSet (BaseSet : Set.S)
    (SetOrd : functor (S : Set.S) -> Set.OrderedType) =
  Set.Make (SetOrd (BaseSet))
module Id (S : Set.S) = S
let () = let module IntSetSet = PowerSet (IntSet) (Id) in ()
