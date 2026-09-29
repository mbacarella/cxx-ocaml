module F ( X : Set.OrderedType ) = struct module Mod : sig module XSet :
  Set.OrderedType end = struct module XSet = String end module Mod2 : sig module
  XSet : Set.OrderedType end = struct module XSet = String end end
