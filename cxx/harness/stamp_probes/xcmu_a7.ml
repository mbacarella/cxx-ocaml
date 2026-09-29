(* the callee reaches the application through a let binding *)
let f g = let h = g in h 1
