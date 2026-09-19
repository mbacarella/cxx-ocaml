module Mod : sig module XSet : Set.OrderedType end = struct module XSet = String
  end module M2 : sig module XSet : Set.OrderedType end = struct module XSet =
  Mod.XSet end
