module F ( X : Set.OrderedType ) = struct module A : Set.OrderedType = String
  module Mod : sig module XSet : Set.OrderedType end = struct module XSet =
  String end end
