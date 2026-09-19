module F ( X : sig end ) = struct module Mod : sig module XSet : Set.S end =
  struct module XSet = Set.Make( String ) end module Mod2 : sig module XSet :
  Set.S end = struct module XSet = Set.Make( String ) end end
