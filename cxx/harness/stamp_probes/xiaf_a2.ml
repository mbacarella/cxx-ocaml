module A = struct let x = 1 end
module B = struct let y = 2 end
module S = Map.Make(String)
include S
let m = add "a" 1 empty
