module F ( X : sig end ) = struct module Mod : sig module XSet : Set.OrderedType
  end = struct module XSet = String end end
