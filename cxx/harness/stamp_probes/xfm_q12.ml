let r = ref 0;;
let s = "" in
  let o' : <m:int> = Marshal.from_string s 0 in
  let o'' : <m:int> = Marshal.from_string s 0 in
  ignore (o', o'');;
