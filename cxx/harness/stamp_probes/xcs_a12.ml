module X = struct type t = A | B  type r = { x : int } end
module Y = struct include X end
