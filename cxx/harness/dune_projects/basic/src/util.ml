let sum = List.fold_left ( +. ) 0.
module M = Map.Make (String)
let count words = List.fold_left (fun m w -> M.update w (function None -> Some 1 | Some n -> Some (n + 1)) m) M.empty words
