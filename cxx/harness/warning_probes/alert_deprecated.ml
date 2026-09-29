module M : sig val x : int [@@deprecated "use y"] end = struct let x = 1 end
let z = M.x
