# Exercise both sources of replacement input: an existing file and bytes
# produced by earlier planned steps. Neither failure can publish the marker.
set(bundle "${root}/text-limits")
set(work "${root}/text-limit-work")
file(MAKE_DIRECTORY "${bundle}" "${work}")
set(modules main.mjs)
set(payloads seed)
set(caps copy replace)
file(WRITE "${bundle}/seed" "seed")
function(reject_text_step expected_step expected_reason)
  seal()
  foreach(mode preview apply)
    invoke(${mode} 1)
    string(JSON reason GET "${report}" error)
    string(JSON completed GET "${report}" completed)
    string(JSON where GET "${report}" detail)
    if(NOT reason STREQUAL expected_reason OR NOT completed EQUAL 0 OR
       NOT where STREQUAL "main.mjs[${expected_step}]" OR EXISTS "${work}/marker")
      message(FATAL_ERROR "${mode} did not preflight the text operation: ${report}")
    endif()
  endforeach()
endfunction()
file(WRITE "${bundle}/main.mjs" [=[
export default () => [
  {op:'copy',payload:'seed',path:'marker'},
  {op:'replace',path:'existing',from:'A',to:'B',count:1}
];
]=])
string(REPEAT "x" 16777217 large)
file(WRITE "${work}/existing" "${large}")
unset(large)
reject_text_step(2 text-limit)
string(ASCII 192 175 malformed)
file(WRITE "${work}/existing" "A${malformed}")
reject_text_step(2 invalid-utf8)

# The first replacement yields exactly the allowed 16 MiB; the second
# would grow it beyond the cap. On Windows both spellings name one file.
string(REPEAT "X" 4096 seed)
file(WRITE "${bundle}/seed" "${seed}")
set(spelling marker)
if(WIN32)
  set(spelling MARKER)
endif()
file(WRITE "${bundle}/main.mjs"
  "export default () => [{op:'copy',payload:'seed',path:'marker'},
   {op:'replace',path:'marker',from:'X',to:'z'.repeat(4095)+'!',count:4096},
   {op:'replace',path:'${spelling}',from:'!',to:'!!',count:4096}];")
reject_text_step(3 text-limit)
