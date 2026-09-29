module W = struct module F ( X : Set.OrderedType ) = struct module rec Mod : sig
  type elt = X.t type t = elt list val compare: t -> t -> int end = struct type
  elt = X.t type t = elt list let compare = (fun x y -> 0) end and ModSet :
  Set.S with type elt = Mod.t = Set.Make( Mod ) end end
