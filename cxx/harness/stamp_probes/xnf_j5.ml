module W = struct module F ( X : Set.OrderedType ) = struct module M : sig
  module N : sig type u = Set.Make( X ).t end end = struct module N = struct
  type u = Set.Make( X ).t end end end end
