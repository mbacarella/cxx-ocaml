module W = struct module F ( X : Set.OrderedType ) = struct module rec Mod : sig
  module XSet : sig type elt = X.t type t = Set.Make( X ).t end type elt = X.t
  type t = XSet.t val compare: t -> t -> int end = struct module XSet =
  Set.Make( X ) type elt = X.t type t = XSet.t let compare = (fun x y -> 0) end
  end end
