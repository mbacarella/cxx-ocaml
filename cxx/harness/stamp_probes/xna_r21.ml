module F ( X : Set.OrderedType ) = struct module Mod : sig module XSet : sig
  type t end end = struct module XSet = Set.Make( String ) end end
