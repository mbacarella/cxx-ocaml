module W = struct module F ( X : Set.OrderedType ) = struct module M : sig
  module XSet : sig type elt = X.t type t = Set.Make( X ).t end end = struct
  module XSet = Set.Make( X ) end type u = M.XSet.t end end
