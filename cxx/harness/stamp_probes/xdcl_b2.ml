module A = struct type t = R of int | S end
module B = struct include A end
