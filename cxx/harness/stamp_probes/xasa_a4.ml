module A = struct type t = int let x = 1 let y = 2 end
module type S = sig val x : int end
module O = struct module B : S = A end
