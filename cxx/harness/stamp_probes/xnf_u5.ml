module W = struct module F ( X : Set.OrderedType ) = struct module rec M : sig
  module XSet : sig type t end end = struct module XSet = Set.Make( X ) end end
  end
