module A = struct type r = { a : int; b : string } end
module B = struct include A end
