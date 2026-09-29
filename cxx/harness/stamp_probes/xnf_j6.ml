module W = struct module F ( X : Set.OrderedType ) = struct module type T = sig
  module N : sig type u = Set.Make( X ).t end end end end
