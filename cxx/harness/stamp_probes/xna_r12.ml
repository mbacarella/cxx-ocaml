module type T = sig module XSet : Set.OrderedType end module Mod : T = struct
  module XSet = String end
