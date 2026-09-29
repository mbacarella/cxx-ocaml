module M : sig val x : int [@@alert foo "bar"] end = struct let x = 1 end
let y = M.x
