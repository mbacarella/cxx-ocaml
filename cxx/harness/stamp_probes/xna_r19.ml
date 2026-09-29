module F ( X : Set.OrderedType ) = struct module Mod : sig module XSet : Set.S
  end = struct module XSet = Set.Make( String ) end end
