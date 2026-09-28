(* an if-then-else ends at its last branch's last token, a trailing
   attribute included (-g event locations) *)
let g x = x
let f b acc = if b then g acc else g acc [@nontail]
let h b acc = if b then g acc [@nontail]
