module F ( X : Set.OrderedType ) = struct module Mod : sig module XSet :
  Set.OrderedType end = struct module XSet = X end end
