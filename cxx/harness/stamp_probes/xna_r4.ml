module F ( X : Set.OrderedType ) = struct module rec Mod : sig module XSet : sig
  type t end type u = XSet.t end = struct module XSet = struct type t = int end
  type u = XSet.t end end
