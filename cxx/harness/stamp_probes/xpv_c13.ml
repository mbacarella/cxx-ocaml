type z = int
module type S = sig type t val to_string : t -> string val x : t end
module M : S = struct type t = int let to_string = string_of_int let x = 1 end
let forget () = let module N = struct include M let x = x end in (module N : S)
