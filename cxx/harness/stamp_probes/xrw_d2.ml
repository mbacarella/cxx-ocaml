module F ( X : Set.OrderedType ) = struct
  module rec Mod : sig
    type t
    val compare: t -> t -> int
  end = struct
    type t = int
    let compare = (fun x y -> 0)
  end
  and ModSet : Set.S with type elt = Mod.t = Set.Make( Mod )
end
