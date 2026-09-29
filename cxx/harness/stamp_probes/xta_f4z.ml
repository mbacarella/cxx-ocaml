module TT : sig module N : sig val x : int val y : int end end = struct
  module N = struct let x = 1 let y = 2 let w = 3 end end
